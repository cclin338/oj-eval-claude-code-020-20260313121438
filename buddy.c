#include "buddy.h"
#define NULL ((void *)0)

#define MAX_RANK 16
#define PAGE_SIZE 4096  // 4K
#define MAX_PAGES 65536  // Maximum possible pages (256MB / 4K)

// Structure for a doubly-linked free list node
typedef struct free_node {
    struct free_node *next;
    struct free_node *prev;
} free_node_t;

// Free lists for each rank (head pointers)
static free_node_t *free_lists[MAX_RANK + 1];

// Base address of managed memory
static void *base_addr = NULL;

// Total number of pages (in 4K units)
static int total_pages = 0;

// Allocation bitmap:
// alloc_map[i] > 0: allocated with that rank
// alloc_map[i] = 0: not the start of a block
// alloc_map[i] < 0: free block start with rank = -alloc_map[i]
static char alloc_map[MAX_PAGES];

// Helper function to calculate power of 2
static inline int pow2(int rank) {
    return 1 << (rank - 1);
}

// Get the index of a page address (in 4K units)
static inline int get_page_index(void *p) {
    long offset = (char *)p - (char *)base_addr;
    return offset / PAGE_SIZE;
}

// Get address from page index
static inline void *get_page_addr(int idx) {
    return (char *)base_addr + (long)idx * PAGE_SIZE;
}

// Get buddy index for a block
static inline int get_buddy_index(int idx, int rank) {
    int block_size = pow2(rank);
    return idx ^ block_size;
}

// Check if a block is aligned for the given rank
static inline int is_aligned(int idx, int rank) {
    int block_size = pow2(rank);
    return (idx & (block_size - 1)) == 0;
}

// Add node to free list
static inline void list_add(free_node_t **head, free_node_t *node) {
    node->next = *head;
    node->prev = NULL;
    if (*head != NULL) {
        (*head)->prev = node;
    }
    *head = node;
}

// Remove node from free list
static inline void list_remove(free_node_t **head, free_node_t *node) {
    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        *head = node->next;
    }
    if (node->next != NULL) {
        node->next->prev = node->prev;
    }
}

// Mark block as free in alloc_map
static inline void mark_free(int idx, int rank) {
    alloc_map[idx] = -rank;
    int block_size = pow2(rank);
    for (int i = 1; i < block_size; i++) {
        alloc_map[idx + i] = 0;
    }
}

// Mark block as allocated in alloc_map
static inline void mark_allocated(int idx, int rank) {
    int block_size = pow2(rank);
    for (int i = 0; i < block_size; i++) {
        alloc_map[idx + i] = rank;
    }
}

// Clear block in alloc_map
static inline void mark_cleared(int idx, int rank) {
    int block_size = pow2(rank);
    for (int i = 0; i < block_size; i++) {
        alloc_map[idx + i] = 0;
    }
}

// Check if buddy is free at the given rank
static inline int is_buddy_free_at_rank(int buddy_idx, int rank) {
    if (buddy_idx < 0 || buddy_idx >= total_pages) return 0;
    return alloc_map[buddy_idx] == -rank;
}

// Initialize the buddy system
int init_page(void *p, int pgcount) {
    if (p == NULL || pgcount <= 0) return -EINVAL;

    base_addr = p;
    total_pages = pgcount;

    // Initialize free lists
    for (int i = 0; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }

    // Initialize allocation map
    for (int i = 0; i < total_pages; i++) {
        alloc_map[i] = 0;
    }

    // Add blocks to free lists, starting from largest possible
    int remaining_pages = pgcount;
    int current_idx = 0;

    while (remaining_pages > 0) {
        int rank = MAX_RANK;

        while (rank >= 1) {
            int block_size = pow2(rank);
            if (block_size <= remaining_pages && is_aligned(current_idx, rank)) {
                // Add this block to free list
                free_node_t *node = (free_node_t *)get_page_addr(current_idx);
                list_add(&free_lists[rank], node);

                // Mark as free
                mark_free(current_idx, rank);

                current_idx += block_size;
                remaining_pages -= block_size;
                break;
            }
            rank--;
        }
    }

    return OK;
}

// Allocate pages with specified rank
void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return ERR_PTR(-EINVAL);
    }

    // Find the smallest available rank >= requested rank
    int current_rank = rank;
    while (current_rank <= MAX_RANK && free_lists[current_rank] == NULL) {
        current_rank++;
    }

    if (current_rank > MAX_RANK) {
        return ERR_PTR(-ENOSPC);
    }

    // Remove block from free list
    free_node_t *block = free_lists[current_rank];
    list_remove(&free_lists[current_rank], block);

    int idx = get_page_index(block);
    mark_cleared(idx, current_rank);

    // Split blocks down to requested rank
    while (current_rank > rank) {
        current_rank--;
        int block_size = pow2(current_rank);

        // Split: add second half (buddy) to free list
        int buddy_idx = idx + block_size;
        void *buddy = get_page_addr(buddy_idx);
        free_node_t *buddy_node = (free_node_t *)buddy;
        list_add(&free_lists[current_rank], buddy_node);

        // Mark buddy as free
        mark_free(buddy_idx, current_rank);
    }

    // Mark block as allocated
    mark_allocated(idx, rank);

    return (void *)block;
}

// Return pages to buddy system
int return_pages(void *p) {
    if (p == NULL) return -EINVAL;

    int idx = get_page_index(p);
    if (idx < 0 || idx >= total_pages) return -EINVAL;

    // Get rank from alloc_map
    int rank = alloc_map[idx];
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;

    // Check if properly aligned
    if (!is_aligned(idx, rank)) return -EINVAL;

    int block_size = pow2(rank);

    // Verify all pages in block have same rank
    for (int i = 1; i < block_size; i++) {
        if (alloc_map[idx + i] != rank) return -EINVAL;
    }

    // Clear allocation map for current block
    mark_cleared(idx, rank);

    // Try to merge with buddy iteratively
    while (rank < MAX_RANK) {
        int buddy_idx = get_buddy_index(idx, rank);

        // Check bounds
        if (buddy_idx < 0 || buddy_idx >= total_pages) break;
        if (buddy_idx + block_size > total_pages) break;

        // Check if buddy is free at this rank using O(1) lookup
        if (!is_buddy_free_at_rank(buddy_idx, rank)) break;

        // Remove buddy from free list in O(1) time using doubly-linked list
        void *buddy_addr = get_page_addr(buddy_idx);
        free_node_t *buddy_node = (free_node_t *)buddy_addr;
        list_remove(&free_lists[rank], buddy_node);

        // Clear buddy's free marker
        mark_cleared(buddy_idx, rank);

        // Merge: new block starts at lower address
        if (buddy_idx < idx) {
            idx = buddy_idx;
        }

        rank++;
        block_size = pow2(rank);
    }

    // Add merged block to free list
    void *block_addr = get_page_addr(idx);
    free_node_t *node = (free_node_t *)block_addr;
    list_add(&free_lists[rank], node);

    // Mark as free
    mark_free(idx, rank);

    return OK;
}

// Query the rank of a page
int query_ranks(void *p) {
    if (p == NULL) return -EINVAL;

    int idx = get_page_index(p);
    if (idx < 0 || idx >= total_pages) return -EINVAL;

    int val = alloc_map[idx];

    // If allocated, return its rank
    if (val > 0 && val <= MAX_RANK) return val;

    // If it's a free block start, return its rank
    if (val < 0) return -val;

    // val == 0 means it's inside a block (not the start)
    // Need to find which block it belongs to
    for (int r = MAX_RANK; r >= 1; r--) {
        int block_size = pow2(r);
        int block_start = (idx / block_size) * block_size;

        if (!is_aligned(block_start, r)) continue;
        if (block_start + block_size > total_pages) continue;

        int start_val = alloc_map[block_start];

        // Check if it's a free block of this rank
        if (start_val == -r) {
            return r;
        }

        // Check if it's an allocated block of this rank
        if (start_val == r) {
            // Verify all pages have same rank
            int valid = 1;
            for (int i = 0; i < block_size; i++) {
                if (alloc_map[block_start + i] != r) {
                    valid = 0;
                    break;
                }
            }
            if (valid) return r;
        }
    }

    return -EINVAL;
}

// Query count of available pages at specified rank
int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;

    int count = 0;
    free_node_t *curr = free_lists[rank];

    while (curr != NULL) {
        count++;
        curr = curr->next;
    }

    return count;
}
