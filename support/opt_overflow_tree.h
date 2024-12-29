/*
 * This file contains a modified version btree
 * 
 */


#pragma once

#include <cassert>
#include <cstring>
#include <atomic>
#include <immintrin.h>
#include <sched.h>
#include <utility>
#include <vector>
#include "typedefine.h"
#include "concurrent.h"

enum class optTreeNodeType : uint8_t {
  BTreeInner = 1, BTreeLeaf = 2
};

static const uint64_t ChildNum = 32;

class OptNodeBase{
public:
  ol_lock nodelock;
  optTreeNodeType type;
  uint16_t count;
  // child array
  uint64_t keys[ChildNum]; 
  void* pointers[ChildNum]; 
  void* smallnode;
  OptNodeBase(){
    type = optTreeNodeType::BTreeLeaf;
    count = 0;
  }
  OptNodeBase(bool is_leaf){
    count = 0;
    if(is_leaf)
      type = optTreeNodeType::BTreeLeaf;
    else
      type = optTreeNodeType::BTreeInner;
  }
  bool isFull(){
    return count == ChildNum;
  }
  unsigned lowerBound(uint64_t k) {
    unsigned lower = 0;
    unsigned upper = count;
    do {
      unsigned mid = ((upper - lower) / 2) + lower;
      // This is the key at the pivot position
      const uint64_t &middle_key = keys[mid];

      if (k < middle_key) {
        upper = mid;
      } else if (k > middle_key) {
        lower = mid + 1;
      } else {
        return mid;
      }
    } while (lower < upper);
    return lower;
  }

  unsigned lowerBoundInner(uint64_t k) {
    unsigned lower = 0;
    unsigned upper = count;
    do {
      unsigned mid = ((upper - lower) / 2) + lower;

      if (k < keys[mid]) {
        upper = mid;
      } else if (k >= keys[mid]) {
        lower = mid + 1;
      } 
    } while (lower < upper);
    return lower;
  }

  bool insertLeaf(uint64_t& k, string_payload& p) {
    if(type == optTreeNodeType::BTreeInner)
      return false;
    assert(count < ChildNum);
    string_payload val = p;
    if (count) {
      unsigned pos = lowerBound(k);
      if ((pos < count) && (keys[pos] == k)) {
        return false;
      }
      memmove(keys + pos + 1, keys + pos, sizeof(uint64_t) * (count - pos));
      memmove(pointers +pos+1,pointers+pos,sizeof(void*)*(count-pos));
      
      keys[pos] = k;
      pointers[pos] = &val;
    } else {
      keys[0] = k;
      pointers[0] = &val;
    }
    count++;
    return true;
  }

  bool searchLeaf(uint64_t& k, string_payload& p){
    assert(count <= ChildNum);
    if (count) {
      unsigned pos = lowerBound(k);
      if ((pos < count) && (keys[pos] == k)) {
        // p = *(static_cast<string_payload*>(pointers[pos]));
        return true;
      }
    } 
    return false;
  }

  bool updateLeaf(uint64_t& k, string_payload& p){
    assert(count <= ChildNum);
    if (count) {
      unsigned pos = lowerBound(k);
      if ((pos < count) && (keys[pos] == k)) {
        string_payload val = p;
        pointers[pos] = &val;
        return true;
      }
    } 
    return false;
  }

};

class OptBTree {
public:
  std::atomic<OptNodeBase *> root;

  OptBTree() {
    root = new OptNodeBase();
  }

  bool clear(){
    root = new OptNodeBase();
    return true;
  }

  bool insert(uint64_t k, string_payload& v) {

    // Current node
    OptNodeBase *node = root;
  OPTETREE_RECHECK:
    while (node->type == optTreeNodeType::BTreeInner) {
      if(k <= node->keys[0]){
        return false;
      }else{
        int pos = node->lowerBoundInner(k) - 1;
        if(pos < 0)
          return false;
        node = static_cast<OptNodeBase*>(node->pointers[pos]);
      }
    }
    
    // lock
    node->nodelock.get_lock();
    if(node->type == optTreeNodeType::BTreeInner){
      node->nodelock.release_lock();
      goto OPTETREE_RECHECK;
    }

    if (node->count == ChildNum) {
      int nextinsertchild_id = 0;
      for(int i = 0; i < ChildNum; i++){
        OptNodeBase* newnode = new OptNodeBase(); 
        newnode->keys[0] = node->keys[i];
        newnode->pointers[0] = node->pointers[i];
        newnode->count = 1;
        newnode->type = optTreeNodeType::BTreeLeaf;
        node->pointers[i] = static_cast<void*>(newnode);

        if(k > node->keys[i])
          nextinsertchild_id = i;
      }
      node->type = optTreeNodeType::BTreeInner;
      // check current node, and find child node
      OptNodeBase* insertnode;
      if(k > node->keys[0]){
        insertnode = static_cast<OptNodeBase*>(node->pointers[nextinsertchild_id]);
        auto ret = insertnode->insertLeaf(k, v);
      }
      node->nodelock.release_lock();
    }else{
      auto ret = node->insertLeaf(k, v);
      node->nodelock.release_lock();
    }
    
    // unlock

    return true;     
  }
  
  bool insertFromNode(uint64_t& k, string_payload& v, OptNodeBase* node){
  OPTETREE_RECHECK2:
    while (node->type == optTreeNodeType::BTreeInner) {
      if(k <= node->keys[0]){
        return false;
      }else{
        int pos = node->lowerBoundInner(k) - 1;
        if(pos < 0)
          return false;
        node = static_cast<OptNodeBase*>(node->pointers[pos]);
      }
    }


    // lock
    node->nodelock.get_lock();
    if(node->type == optTreeNodeType::BTreeInner){
      node->nodelock.release_lock();
      goto OPTETREE_RECHECK2;
    }

    if (node->count == ChildNum) {
      int nextinsertchild_id = 0;
      for(int i = 0; i < ChildNum; i++){
        OptNodeBase* newnode = new OptNodeBase();
        newnode->keys[0] = node->keys[i];
        newnode->pointers[0] = node->pointers[i];
        newnode->count = 1;
        node->pointers[i] = static_cast<void*>(newnode);
        newnode->type = optTreeNodeType::BTreeLeaf;

        if(k > node->keys[i])
          nextinsertchild_id = i;
      }
      node->type = optTreeNodeType::BTreeInner;

      OptNodeBase* insertnode;

      if(k > node->keys[0]){
        insertnode = static_cast<OptNodeBase*>(node->pointers[nextinsertchild_id]);
        auto ret = insertnode->insertLeaf(k, v);
      }
      node->nodelock.release_lock();
    }else{
      auto ret = node->insertLeaf(k, v);
      node->nodelock.release_lock();
    }
    
    // unlock

    return true;     
  }

  bool update(uint64_t k, string_payload& new_payload) {
    // Current node
    OptNodeBase *node = root;
    uint32_t version;
    bool ret;
  OPTTREE_REGET:
    while (node->type == optTreeNodeType::BTreeInner) {
      if(k < node->keys[0]){
        node = static_cast<OptNodeBase*>(node->smallnode);
      }else{
        node = static_cast<OptNodeBase*>(node->pointers[node->lowerBoundInner(k) - 1]);
      }
    }

    
    node->nodelock.get_lock();
    ret = node->updateLeaf(k, new_payload);
    node->nodelock.release_lock();

    return ret;
  }

  bool lookup(uint64_t k, string_payload& result) {
    // Current node
    OptNodeBase *node = root;
    uint32_t version;
    bool ret;
  OPTTREE_REGET:
    while (node->type == optTreeNodeType::BTreeInner) {
      if(k < node->keys[0]){
        node = static_cast<OptNodeBase*>(node->smallnode);
      }else{
        node = static_cast<OptNodeBase*>(node->pointers[node->lowerBoundInner(k) - 1]);
      }
    }

    
    if (node->nodelock.test_lock_set(version)){
      while(node->nodelock.test_lock_set(version)){
      };
    }
    ret = node->searchLeaf(k, result);
    if (node->nodelock.test_lock_version_change(version)){
      goto OPTTREE_REGET;
    }

    return ret;
  }

  bool lookupFromNode(uint64_t k, string_payload& result, OptNodeBase* node){
    uint32_t version;
    bool ret;
  OPTTREE_REGET2:
    while (node->type == optTreeNodeType::BTreeInner) {
      if(k < node->keys[0]){
        node = static_cast<OptNodeBase*>(node->smallnode);
      }else{
        node = static_cast<OptNodeBase*>(node->pointers[node->lowerBoundInner(k) - 1]);
      }
    }

    
    if (node->nodelock.test_lock_set(version)){
      while(node->nodelock.test_lock_set(version)){
      };
    }
    ret = node->searchLeaf(k, result);
    if (node->nodelock.test_lock_version_change(version)){
      goto OPTTREE_REGET2;
    }

    return ret;
  }

  void helpGetAllInner(OptNodeBase* node, std::vector<uint64_t>& retkeys, std::vector<void*>& retpoints){
    if(node->type == optTreeNodeType::BTreeLeaf){
      return;
    }
    for(int i = 0; i < node->count; i++){
      auto nextnode = static_cast<OptNodeBase*>(node->pointers[i]);
      if(nextnode->type != optTreeNodeType::BTreeLeaf){
        helpGetAllInner(nextnode, retkeys, retpoints);
      }else{
        retkeys.push_back(node->keys[i]);
        retpoints.push_back((void*)(node->pointers[i]));
      }
    }
  }

  bool getAllInner(std::vector<uint64_t>& retkeys, std::vector<void*>& retpoints){
    OptNodeBase *node = root;
    if(node->type == optTreeNodeType::BTreeLeaf){
      return false;
    }
    helpGetAllInner(root, retkeys, retpoints);
    return true;
  }

  void helpGetAllData(OptNodeBase* node, std::vector<uint64_t>& retkeys, std::vector<string_payload>& retvals){
    if(node->type == optTreeNodeType::BTreeLeaf){
      for(int i = 0; i < node->count; i++){
        retkeys.push_back(node->keys[i]);
        retvals.push_back(string_payload(0));
      }
      return;
    }
    for(int i = 0; i < node->count; i++){
      auto nextnode = static_cast<OptNodeBase*>(node->pointers[i]);
      if(nextnode->type != optTreeNodeType::BTreeLeaf){
        helpGetAllData(nextnode, retkeys, retvals);
      }
    }
  }

  bool getAllData(std::vector<uint64_t>& retkeys, std::vector<string_payload>& retvals){
    OptNodeBase *node = root;
    if(node->type == optTreeNodeType::BTreeLeaf){
      return false;
    }
    helpGetAllData(root, retkeys, retvals);
    return true;
  }

  void helpGetDataWithinRange( OptNodeBase* node, uint64_t beginKey, uint64_t endKey, std::vector<std::pair<uint64_t, string_payload>> retkvs, int &offset){
    if(node->type == optTreeNodeType::BTreeLeaf){
      for(int i = 0; i < node->count; i++){
        if(node->keys[i] >= beginKey && node->keys[i] <= endKey){
          retkvs[offset] = {node->keys[i], string_payload(0)};
          offset++;
        }
      }
      return;
    }
    for(int i = 0; i < node->count; i++){
      auto nextnode = static_cast<OptNodeBase*>(node->pointers[i]);
      if(nextnode->type != optTreeNodeType::BTreeLeaf){
        helpGetDataWithinRange(nextnode, beginKey, endKey, retkvs, offset);
      }
    }
  }

  bool getAllDataWithinRange(uint64_t beginKey, uint64_t endKey, std::vector<std::pair<uint64_t, string_payload>> retkvs, int &offset){
    OptNodeBase *node = root;
    if(node->type == optTreeNodeType::BTreeLeaf){
      return false;
    }
    helpGetDataWithinRange(root, beginKey, endKey, retkvs, offset);
    return true;
  }

};
