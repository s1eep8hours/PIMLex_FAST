#ifndef _COMMON_H_
#define _COMMON_H_


// Data type
#define DTYPE uint64_t

// max PGM level number
#define MAX_LEVEL 5

#define USE_LUT 0
// max model number in DPU
#if USE_LUT 
 #define MAX_MODEL_SIZE 512 // normal: 2560, enable LUT: 512
#else
 #define MAX_MODEL_SIZE 2560
#endif

// max buffer size
#define MAX_BUFFER_SIZE 1 << 15

// buffer struct
typedef struct {
	int n_tasks;
	int op_type;
	DTYPE sbuffer[MAX_BUFFER_SIZE];
}send_buffer_t;

typedef struct{
	uint32_t endkeys[MAX_BUFFER_SIZE];
}scan_end_keys_t;

typedef struct{
	int n_tasks;
	int padding;
	uint32_t rbuffer[MAX_BUFFER_SIZE];
}recv_buffer_t;

typedef struct{
	int64_t n_tasks;
	uint64_t pointer_rbuffer[MAX_BUFFER_SIZE];
}overflow_recv_buffer_t;

// overflow tree struct
#define MAX_OVERFLOW_KEY_SIZE 1 << 19
typedef struct 
{
	DTYPE realinnerkeys[MAX_OVERFLOW_KEY_SIZE];
}overflow_keys_t;

typedef struct 
{
	uint64_t realinnerpointers[MAX_OVERFLOW_KEY_SIZE];
}overflow_pointers_t;

// learned parameter
#define EpsilonRecursive_dpu 64
#define DataEpsilon 64 // 512

#define INVAILD_POS 1000000000U

typedef struct {
	uint64_t input_size;
	enum kernels {
		kernel1 = 0,
		nr_kernels = 1,
	} kernel;
	int max_levels; // level_offset[max_level] is the end of top level offset
	int level_offset[MAX_LEVEL];
	uint32_t start_pos;
	size_t setEpsilon;
} dpu_arguments_t;

// Structures used by dpu to store model
typedef struct {
    DTYPE key;             ///< The first key that the segment indexes.
    float slope;    ///< The slope of the segment.
    int32_t intercept; ///< The intercept of the segment.
} pgm_model_t;

const int32_t access_wram_cost = 4; // plus other ops (if/else etc..)
const int32_t prediction_cost = 45; // 40 + log(eplison) * access_cost + other
const int32_t compute_cost = 40;
const int32_t access_mram_cost = 20; // binary search + access mram

#define MAX_LUT_SIZE 16
#define MORE_SHIFT_NUM 4
typedef struct {
	int32_t use_lut;
	uint32_t shift_len;
	int32_t pos[MAX_LUT_SIZE + 1]; 
	uint32_t padding;
} lut_model_t;

#ifndef ENERGY
#define ENERGY 0
#endif
#define PRINT 0

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_RESET   "\x1b[0m"
#endif
