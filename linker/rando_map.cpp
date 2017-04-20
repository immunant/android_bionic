// Copyright (c) 2016 Immunant, Inc.

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "private/bionic_prctl.h"
#include "private/libc_logging.h"

#include "linker.h"
#include "rando_map.h"

static const uintptr_t kMapVersion = 1;

struct RandoMapNode {
  uint8_t *div_start;
  uint8_t *div_end;
  uint8_t *undiv_start;
  uint8_t *undiv_vaddr;

  size_t num_funcs;
  RandoMapFunction *funcs;

  RandoMapNode *left;
  RandoMapNode *right;

  // Balancing metadata starts here.
  // Note on current balancing algorithm: we use a
  // randomized BST called a "treap", with the property
  // that the tree is a BST over "div_start" and a max-heap
  // over the "prio" values, which are randomly generated.
  // This should produce a reasonably balanced tree,
  // with high probability.
  uint32_t prio;
};

struct RandoMapHeader {
  uintptr_t version;
  RandoMapNode *root;
};

struct RandoMapPage {
  RandoMapNode *next;
  RandoMapNode *limit;
};

static RandoMapPage current_page = { nullptr, nullptr };
static RandoMapNode *free_list = nullptr;
static RandoMapHeader *rando_map_header = nullptr;

static void *alloc_map_page() {
  void *map_start = mmap(nullptr, PAGE_SIZE,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         0, 0);
  if (map_start == MAP_FAILED)
    __libc_fatal("rando_map mmap failed");

  memset(map_start, 0, PAGE_SIZE);

  uintptr_t map_end = reinterpret_cast<uintptr_t>(map_start) + PAGE_SIZE;
  current_page.next = reinterpret_cast<RandoMapNode*>(map_start);
  current_page.limit = reinterpret_cast<RandoMapNode*>(map_end) - 1;
  return map_start;
}

void rando_map_init() {
  void *map_start = alloc_map_page();
  prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, map_start, PAGE_SIZE, "$$rando_map$$");

  rando_map_header = reinterpret_cast<RandoMapHeader*>(map_start);
  rando_map_header->version = kMapVersion;
  rando_map_header->root = nullptr;

  current_page.next = reinterpret_cast<RandoMapNode*>(rando_map_header + 1);
}

static inline RandoMapNode *alloc_node() {
  RandoMapNode *res;
  if (free_list != nullptr) {
    res = free_list;
    free_list = res->right;
    return res;
  }

  if (current_page.next > current_page.limit)
    alloc_map_page();

  res = current_page.next;
  current_page.next++;
  return res;
}

static inline RandoMapNode *rotate_left_son(RandoMapNode **ptr) {
  RandoMapNode *curr = *ptr;
  RandoMapNode *son = curr->left;
  RandoMapNode *gson = son->right;
  // Now do the rotation
  son->right = curr;
  curr->left = gson;
  *ptr = son;
  return son;
}

static inline RandoMapNode *rotate_right_son(RandoMapNode **ptr) {
  RandoMapNode *curr = *ptr;
  RandoMapNode *son = curr->right;
  RandoMapNode *gson = son->left;
  // Now do the rotation
  son->left = curr;
  curr->right = gson;
  *ptr = son;
  return son;
}

static void map_tree_insert_node(RandoMapNode *node,
                                 RandoMapNode **ptr) {
  RandoMapNode *curr = *ptr;
  if (curr == nullptr) {
    *ptr = node;
    return;
  }

  if (node->div_start < curr->div_start) {
    map_tree_insert_node(node, &curr->left);
    if (curr->left->prio > curr->prio)
      rotate_left_son(ptr);
  } else if (node->div_start >= curr->div_end) {
    map_tree_insert_node(node, &curr->right);
    if (curr->right->prio > curr->prio)
      rotate_right_son(ptr);
  } else {
    __libc_fatal("overlapping rando map nodes");
  }
}

static RandoMapNode *map_tree_delete_node(uint8_t *div_start,
                                          RandoMapNode **ptr) {
  RandoMapNode *curr = *ptr;
  if (curr == nullptr)
    __libc_fatal("trying to delete inexistent node");

  if (div_start < curr->div_start) {
    return map_tree_delete_node(div_start, &curr->left);
  } else if (div_start >= curr->div_end) {
    return map_tree_delete_node(div_start, &curr->right);
  } else {
    // Found it!!!
    curr->prio = 0;
    // TODO: this could be a loop (if the compiler doesn't do TCO)
    if (curr->left == nullptr) {
      // Leaf or single child => just take it out directly
      *ptr = curr->right;
      return curr;
    }
    if (curr->right == nullptr) {
      // Leaf or single child => just take it out directly
      *ptr = curr->left;
      return curr;
    }
    if (curr->left->prio > curr->right->prio) {
      RandoMapNode *son = rotate_left_son(ptr);
      return map_tree_delete_node(div_start, &son->right);
    } else {
      RandoMapNode *son = rotate_right_son(ptr);
      return map_tree_delete_node(div_start, &son->left);
    }
  }
}

void rando_map_add(uint8_t *div_start, size_t div_size,
                   uint8_t *undiv_start, uint8_t *undiv_vaddr,
                   size_t num_funcs, RandoMapFunction *funcs) {
  RandoMapNode *node = alloc_node();
  current_page.next++;

  node->div_start = div_start;
  node->div_end = div_start + div_size;
  node->undiv_start = undiv_start;
  node->undiv_vaddr = undiv_vaddr;
  node->num_funcs = num_funcs;
  node->funcs = funcs;
  node->left = nullptr;
  node->right = nullptr;
  node->prio = arc4random_uniform(UINT32_MAX);

  map_tree_insert_node(node, &rando_map_header->root);
}

void rando_map_delete(uint8_t *div_start) {
  RandoMapNode *node = map_tree_delete_node(div_start, &rando_map_header->root);
  // Put node on the free list
  memset(node, 0, sizeof(RandoMapNode));
  node->right = free_list;
  free_list = node;
}

