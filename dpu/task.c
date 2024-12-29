/*
* pimlex dpu
*
*/
#include <stdint.h>
#include <stdio.h>
#include <defs.h>
#include <mram.h>
#include <alloc.h>
#include <barrier.h>
#include <perfcounter.h>
#include "common.h"
#include <string.h>

int FastLog2(int x)
{
    float fx;
    unsigned long ix, exp;
 
    fx = (float)x;
    ix = *(unsigned long*)&fx;
    exp = (ix >> 23) & 0xFF;
 
    return exp - 127;
}

#define DEBUG 0
#define PRINT 0
#define BLOCK_SIZE 8
#define FETCH_QUERY 32
#define MOV_NUM 3  // 2^MOV_NUM = INTERLEAVE

#define WORD_MASK 0xfffffff8
__host dpu_arguments_t DPU_INPUT_ARGUMENTS;
__mram_noinit send_buffer_t dpu_sbuffer;
__mram_noinit recv_buffer_t dpu_recv_buffer;
__host int64_t overflow_len;
__host int32_t start_level;
__mram_noinit overflow_keys_t dpu_overflow_keys;
__mram_noinit overflow_pointers_t dpu_overflow_pointers;
__mram_noinit overflow_recv_buffer_t dpu_overflow_recv_buffer;
__host pgm_model_t model_array[MAX_MODEL_SIZE];
#if USE_LUT
__host lut_model_t lut_model_array[MAX_MODEL_SIZE];
#endif

uint32_t start_mram_input_addr;

BARRIER_INIT(my_barrier, NR_TASKLETS);

extern int main_kernel1(void);

int(*kernels[nr_kernels])(void) = {main_kernel1};

int main(void){
  // Kernel
  return kernels[DPU_INPUT_ARGUMENTS.kernel]();
}

// main_kernel1
int main_kernel1() {
  unsigned int tasklet_id = me();
  #if PRINT
  printf("tasklet_id = %u\n", tasklet_id);
  #endif

  barrier_wait(&my_barrier);
  

  if(dpu_sbuffer.op_type == 0){
    // INIT
    // init index struct at create
    if(tasklet_id == 0){
      mem_reset(); // Reset the heap
      start_mram_input_addr = (uint32_t) DPU_MRAM_HEAP_POINTER;
      overflow_len = 0;

      start_level = 0;
      for(int level = 0; level < DPU_INPUT_ARGUMENTS.max_levels; level++){
        int level_num = DPU_INPUT_ARGUMENTS.level_offset[level + 1] - DPU_INPUT_ARGUMENTS.level_offset[level];
        if(access_wram_cost * FastLog2(level_num) > prediction_cost + access_wram_cost * FastLog2((int)(2 * DPU_INPUT_ARGUMENTS.setEpsilon))){
           start_level++;
        }else{
          break;
        }
      }

    }
    // Barrier
    barrier_wait(&my_barrier);
  } 
  else if(dpu_sbuffer.op_type == 1){
    // search layer
    DTYPE searching_for;
    DTYPE searching_block[FETCH_QUERY];
    uint32_t current_query_in_block = 0;
    uint32_t num_task = dpu_sbuffer.n_tasks;
    uint32_t current_mram_query = tasklet_id * (num_task / NR_TASKLETS);
    uint32_t total_ops = num_task / NR_TASKLETS;
    if(current_mram_query % 2 != 0){
      current_mram_query--;
      total_ops++;
    }

    if(tasklet_id == NR_TASKLETS - 1){
      total_ops = num_task - current_mram_query;
    }

    if(total_ops % 2 != 0){
      total_ops++;
    }
    dpu_recv_buffer.n_tasks = num_task;

    // init cache for learned search
    DTYPE fetch_value;
    DTYPE check_block[2];
    uint32_t temp_pos[2];
    uint32_t current_temp_op_pos = 0;

    int32_t l,r,mid;
    // predict pos
    int predicted_pos;
    long cur_offset;

    for(uint32_t targets = 0; targets < total_ops; targets++)
    {
      if(current_query_in_block == 0){
        mram_read((__mram_ptr void const *)&(dpu_sbuffer.sbuffer[current_mram_query]), &searching_block, 8 * FETCH_QUERY);
      }
      searching_for = searching_block[current_query_in_block];

      l = DPU_INPUT_ARGUMENTS.level_offset[start_level];
      r = DPU_INPUT_ARGUMENTS.level_offset[start_level +  1]- 1;
      mid = l;
      while(l < r){
        mid = l + (r - l) / 2;
        if(model_array[mid].key <= searching_for){
          l = mid + 1;
        }else{
          r = mid;
        }
      }

      // pgm perdict
      // 1. get top level model
      cur_offset = l - 1;

      // 2. recursive levels
      #if USE_LUT
      for(int cur_level = start_level - 1; cur_level >=0; cur_level--){
        unsigned long level_begin = DPU_INPUT_ARGUMENTS.level_offset[cur_level];
        long pos;
        if(lut_model_array[cur_offset].use_lut){
          uint64_t temp = searching_for >> lut_model_array[cur_offset].shift_len;
          uint64_t temp2 = model_array[cur_offset].key >> lut_model_array[cur_offset].shift_len;
          if(temp - temp2 >= 16){ 
            pos = model_array[cur_offset].slope * (searching_for - model_array[cur_offset].key) + model_array[cur_offset].intercept;
          }else{  
            pos = lut_model_array[cur_offset].pos[temp - temp2];
          }
          if(pos < 0)
            pos = 0ull;
          else
            pos = (unsigned long)(pos);

          // next model
          if(cur_offset < DPU_INPUT_ARGUMENTS.level_offset[DPU_INPUT_ARGUMENTS.max_levels]){
            if(pos > model_array[cur_offset + 1].intercept){
              pos = model_array[cur_offset + 1].intercept;
            }
          }
        }else{
          pos = model_array[cur_offset].slope * (searching_for - model_array[cur_offset].key) + model_array[cur_offset].intercept;
          if(pos < 0)
          pos = 0ull;
          else
          pos = (unsigned long)(pos);

          // next model
          if(cur_offset < DPU_INPUT_ARGUMENTS.level_offset[DPU_INPUT_ARGUMENTS.max_levels]){
            if(pos > model_array[cur_offset + 1].intercept){
              pos = model_array[cur_offset + 1].intercept;
            }
          }
        }

        if(pos > (EpsilonRecursive_dpu + 1)){
          pos = pos - EpsilonRecursive_dpu - 1;
        }else{
          pos = 0;
        }
        int next_level_lo = level_begin + pos; 
        cur_offset = next_level_lo;
        for(; (cur_offset + 1) < DPU_INPUT_ARGUMENTS.level_offset[DPU_INPUT_ARGUMENTS.max_levels] && model_array[cur_offset + 1].key <= searching_for; cur_offset++){
        }
      }
      #else
      for(int cur_level = start_level - 1; cur_level >=0; cur_level--){
        unsigned long level_begin = DPU_INPUT_ARGUMENTS.level_offset[cur_level];
        long pos = model_array[cur_offset].slope * (searching_for - model_array[cur_offset].key) + model_array[cur_offset].intercept;
        if(pos < 0)
         pos = 0ull;
        else
         pos = (unsigned long)(pos);

        // next model
        if(cur_offset < DPU_INPUT_ARGUMENTS.level_offset[DPU_INPUT_ARGUMENTS.max_levels]){
          if(pos > model_array[cur_offset + 1].intercept){
            pos = model_array[cur_offset + 1].intercept;
          }
        }

        if(pos > (EpsilonRecursive_dpu + 1)){
          pos = pos - EpsilonRecursive_dpu - 1;
        }else{
          pos = 0;
        }

        int next_level_lo = level_begin + pos;

        cur_offset = next_level_lo;
        for(; (cur_offset + 1) < DPU_INPUT_ARGUMENTS.level_offset[DPU_INPUT_ARGUMENTS.max_levels] && model_array[cur_offset + 1].key <= searching_for; cur_offset++){
        }
      }
      #endif


      #if USE_LUT
      if(lut_model_array[cur_offset].use_lut){
          uint64_t temp = searching_for >> lut_model_array[cur_offset].shift_len;
          uint64_t temp2 = model_array[cur_offset].key >> lut_model_array[cur_offset].shift_len;
          int predicted_pos2;
          if(temp - temp2 >= 16){ 
            predicted_pos = (int)(model_array[cur_offset].slope * (searching_for - model_array[cur_offset].key)) + model_array[cur_offset].intercept;
            predicted_pos2 = predicted_pos;
          }else{      
            predicted_pos = lut_model_array[cur_offset].pos[temp - temp2];
            predicted_pos2 = lut_model_array[cur_offset].pos[temp - temp2 + 1];
          }

          // if((l < (DPU_INPUT_ARGUMENTS.level_offset[1] - 1)) && (model_array[cur_offset + 1].intercept < predicted_pos2)){
          //   predicted_pos2 = model_array[cur_offset +  1].intercept;
          // }
          if(predicted_pos > (DPU_INPUT_ARGUMENTS.setEpsilon)){
            l = predicted_pos - DPU_INPUT_ARGUMENTS.setEpsilon;
          }else{
            l = 0;
          }
          r = predicted_pos2 + DPU_INPUT_ARGUMENTS.setEpsilon + 2;
          if(r > DPU_INPUT_ARGUMENTS.input_size){
            r = DPU_INPUT_ARGUMENTS.input_size;
          }
      }else{
          predicted_pos = (int)(model_array[cur_offset].slope * (searching_for - model_array[cur_offset].key)) + model_array[cur_offset].intercept;

          if((l < (DPU_INPUT_ARGUMENTS.level_offset[1] - 1)) && (model_array[cur_offset +  1].intercept < predicted_pos)){
            predicted_pos = model_array[l].intercept;
          }

          if(predicted_pos > (int)(DPU_INPUT_ARGUMENTS.setEpsilon)){
            l = predicted_pos - DPU_INPUT_ARGUMENTS.setEpsilon; 
          }else{
            l = 0;
          }
          r = predicted_pos + DPU_INPUT_ARGUMENTS.setEpsilon + 2; 
          if(r >= DPU_INPUT_ARGUMENTS.input_size){
            r = DPU_INPUT_ARGUMENTS.input_size;
          }
      }
      #else
      predicted_pos = (int)(model_array[cur_offset].slope * (searching_for - model_array[cur_offset].key)) + model_array[cur_offset].intercept;
      
      if(((cur_offset + 1) < (DPU_INPUT_ARGUMENTS.level_offset[1] - 1)) && (model_array[cur_offset + 1].intercept < predicted_pos)){
        predicted_pos = model_array[cur_offset + 1].intercept;
      }

      if(predicted_pos > (int)(DPU_INPUT_ARGUMENTS.setEpsilon)){ 
        l = predicted_pos - DPU_INPUT_ARGUMENTS.setEpsilon;
      }else{
        l = 0;
      }
      r = predicted_pos + DPU_INPUT_ARGUMENTS.setEpsilon + 2;
      if(r >= DPU_INPUT_ARGUMENTS.input_size){
        r = DPU_INPUT_ARGUMENTS.input_size;
      }
      #endif


      while(l < r){
        mid = l + (r - l) / 2;
        mram_read((__mram_ptr void const *) (DPU_MRAM_HEAP_POINTER + 8 * mid), &fetch_value, sizeof(DTYPE));
        if(fetch_value <= searching_for){
          l = mid + 1;
        }else{
          r = mid;
        }
      }


      if(l > (DPU_INPUT_ARGUMENTS.input_size - 1)){
        temp_pos[current_temp_op_pos] = INVAILD_POS;
      }else{
        mram_read((__mram_ptr void const *) (DPU_MRAM_HEAP_POINTER + 8 * (l - 1)), &check_block, sizeof(DTYPE) * 2);
        if(check_block[0] <= searching_for && check_block[1] > searching_for){
          temp_pos[current_temp_op_pos] = (uint32_t)(l - 1 + DPU_INPUT_ARGUMENTS.start_pos) << MOV_NUM;// << 2; 
        }else{
          if(l == (DPU_INPUT_ARGUMENTS.input_size - 1)){
            temp_pos[current_temp_op_pos] = (uint32_t)(l - 1 + DPU_INPUT_ARGUMENTS.start_pos) << MOV_NUM;
          }else{
            temp_pos[current_temp_op_pos] = INVAILD_POS;
          }
        }
      }


      current_mram_query ++;

      current_temp_op_pos++;
      if(current_temp_op_pos == 2){
        mram_write((void*)temp_pos, &(dpu_recv_buffer.rbuffer[current_mram_query - 2]), 8);
        current_temp_op_pos = 0;
      }

      current_query_in_block++;
      if(current_query_in_block == FETCH_QUERY)
        current_query_in_block = 0;

    }
    #if DEBUG
      dpu_recv_buffer.n_tasks = search_num;
    #endif
  }else if(dpu_sbuffer.op_type == 2){
    // handle skew insert, overflow tree
    DTYPE searching_for;
    DTYPE fetch_value;
    uint32_t num_task = dpu_sbuffer.n_tasks;
    uint32_t current_mram_query = tasklet_id * (num_task / NR_TASKLETS);
    uint32_t total_ops = num_task / NR_TASKLETS;
    if(tasklet_id == NR_TASKLETS - 1){
      total_ops = num_task - current_mram_query;
    }

    if(overflow_len != 0){
  
      int32_t l,r,mid;
      for(uint32_t targets = 0; targets < total_ops; targets++)
      {
        mram_read((__mram_ptr void const *)&(dpu_sbuffer.sbuffer[current_mram_query]), &searching_for, 8);
        searching_for = dpu_sbuffer.sbuffer[current_mram_query];

        l = 0;
        r = overflow_len -  1;

        while(l <= r){
          mid = l + (r - l) / 2;
          mram_read((__mram_ptr void const *)&(dpu_overflow_keys.realinnerkeys[mid]), &fetch_value, 8);
          if(fetch_value > searching_for){
            r = mid - 1;
          }else{
            l = mid + 1;
          }
        }

        if(r > (DPU_INPUT_ARGUMENTS.input_size - 1)){
          dpu_overflow_recv_buffer.pointer_rbuffer[current_mram_query] = INVAILD_POS;
        }else{
          mram_read((__mram_ptr void const *)&(dpu_overflow_pointers.realinnerpointers[r]), &fetch_value, 8);
          mram_write((void*)(&fetch_value), &(dpu_overflow_recv_buffer.pointer_rbuffer[current_mram_query]), 8);
        }
        current_mram_query++;
      }

      dpu_overflow_recv_buffer.n_tasks = num_task;

    }
    
    if(overflow_len == 0)
      dpu_overflow_recv_buffer.n_tasks = 0;

  } 

  // Barrier
  barrier_wait(&my_barrier);
  return 0;
}
