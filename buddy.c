#include "buddy.h"
#define NULL ((void *)0)

#define MAX_RANK 16
#define PAGE_SIZE 4096  // 4K
#define MAX_PAGES 65536  // Maximum possible pages (256MB / 4K)

// Structure for a free list node
typedef struct free_node {
    struct free_node *next;
} free_node_t;

// Free lists for each rank
static free_node_t *free_lists[MAX_RANK + 1];

// Base address of managed memory
static void *base_addr = NULL;

// Total number of pages (in 4K units)
static int total_pages = 0;

// Allocation bitmap - tracks which blocks are allocated at each rank
// For a page at index i, we store its allocation status (0=free, >0=rank)
static char alloc_map[MAX_PAGES];

// Helper function to calculate power of 2
static int pow2(int rank) {
    return 1 << (rank - 1);
}

// Get the index of a page address (in 4K units)
static int get_page_index(void *p) {
    if (base_addr == NULL || p < base_addr) return -1;
    long offset = (char *)p - (char *)base_addr;
    if (offset < 0 || offset % PAGE_SIZE != 0) return -1;
    int idx = offset / PAGE_SIZE;
    if (idx >= total_pages) return -1;
    return idx;
}

// Get address from page index
static void *get_page_addr(int idx) {
    return (char *)base_addr + (long)idx * PAGE_SIZE;
}

// Get buddy index for a block
static int get_buddy_index(int idx, int rank) {
    int block_size = pow2(rank);
    return idx ^ block_size;
}

// Check if a block is aligned for the given rank
static int is_aligned(int idx, int rank) {
    int block_size = pow2(rank);
    return (idx % block_size) == 0;
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
    for (int i = 0; i < total_pages && i < MAX_PAGES; i++) {
        alloc_map[i] = 0;  // 0 means free
    }

    // Add blocks to free lists, starting from largest possible
    int remaining_pages = pgcount;
    int current_idx = 0;

    while (remaining_pages > 0) {
        int rank = MAX_RANK;
        int found = 0;

        while (rank >= 1) {
            int block_size = pow2(rank);
            if (block_size <= remaining_pages && is_aligned(current_idx, rank)) {
                // Add this block to free list
                free_node_t *node = (free_node_t *)get_page_addr(current_idx);
                node->next = free_lists[rank];
                free_lists[rank] = node;

                current_idx += block_size;
                remaining_pages -= block_size;
                found = 1;
                break;
            }
            rank--;
        }

        if (!found) break;  // Can't place any more blocks
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
        return ERR_PTR(-ENOSPC);  // No space available
    }

    // Remove block from free list
    free_node_t *block = free_lists[current_rank];
    free_lists[current_rank] = block->next;

    // Split blocks down to requested rank
    while (current_rank > rank) {
        current_rank--;
        int block_size = pow2(current_rank);

        // Split: add second half (buddy) to free list
        void *buddy = (char *)block + (long)block_size * PAGE_SIZE;
        free_node_t *buddy_node = (free_node_t *)buddy;
        buddy_node->next = free_lists[current_rank];
        free_lists[current_rank] = buddy_node;
    }

    // Mark block as allocated in alloc_map
    int idx = get_page_index(block);
    if (idx >= 0 && idx < MAX_PAGES) {
        int block_size = pow2(rank);
        for (int i = 0; i < block_size && (idx + i) < MAX_PAGES; i++) {
            alloc_map[idx + i] = rank;
        }
    }

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

    // Verify all pages in block have same rank (means it's a valid allocated block)
    for (int i = 0; i < block_size && (idx + i) < total_pages; i++) {
        if (alloc_map[idx + i] != rank) return -EINVAL;
    }

    // Clear allocation map
    for (int i = 0; i < block_size && (idx + i) < total_pages; i++) {
        alloc_map[idx + i] = 0;
    }

    // Try to merge with buddy
    while (rank < MAX_RANK) {
        int buddy_idx = get_buddy_index(idx, rank);

        // Check if buddy exists in valid range
        if (buddy_idx < 0 || buddy_idx >= total_pages) break;
        if (!is_aligned(buddy_idx, rank)) break;

        // Check if entire buddy block is free
        int is_buddy_free = 1;
        for (int i = 0; i < block_size; i++) {
            if ((buddy_idx + i) >= total_pages || alloc_map[buddy_idx + i] != 0) {
                is_buddy_free = 0;
                break;
            }
        }

        if (!is_buddy_free) break;

        // Find and remove buddy from free list
        void *buddy_addr = get_page_addr(buddy_idx);
        free_node_t **curr = &free_lists[rank];
        int found = 0;

        while (*curr != NULL) {
            if ((void *)(*curr) == buddy_addr) {
                *curr = (*curr)->next;
                found = 1;
                break;
            }
            curr = &((*curr)->next);
        }

        if (!found) break;  // Buddy not in free list, can't merge

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
    node->next = free_lists[rank];
    free_lists[rank] = node;

    return OK;
}

// Query the rank of a page
int query_ranks(void *p) {
    if (p == NULL) return -EINVAL;

    int idx = get_page_index(p);
    if (idx < 0 || idx >= total_pages) return -EINVAL;

    int rank = alloc_map[idx];

    // If allocated, return its rank
    if (rank > 0 && rank <= MAX_RANK) return rank;

    // If free, find maximum rank block containing this page
    for (int r = MAX_RANK; r >= 1; r--) {
        // Calculate the starting index of the block that would contain this page
        int block_size = pow2(r);
        int block_start = (idx / block_size) * block_size;

        // Check if this block is aligned and within bounds
        if (!is_aligned(block_start, r)) continue;
        if (block_start + block_size > total_pages) continue;

        // Check if entire block is free
        int all_free = 1;
        for (int i = 0; i < block_size; i++) {
            if ((block_start + i) >= total_pages || alloc_map[block_start + i] != 0) {
                all_free = 0;
                break;
            }
        }

        if (!all_free) continue;

        // Check if this block exists in free list
        free_node_t *curr = free_lists[r];
        void *target_addr = get_page_addr(block_start);

        while (curr != NULL) {
            if ((void *)curr == target_addr) {
                return r;
            }
            curr = curr->next;
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
