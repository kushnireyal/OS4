#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>

/*---------------DECLARATIONS-----------------------------------*/
#define MAX_ALLOC 100000000
#define MMAP_THRESHOLD 131072

size_t _size_meta_data();

size_t free_blocks, free_bytes, allocated_blocks, allocated_bytes;

struct MallocMetadata {
    size_t size;
    bool is_free;
    bool is_mmap;

    MallocMetadata* next_free; // if not free then null
    MallocMetadata* prev_free; // sorted by size

    MallocMetadata* heap_next; // every block has
    MallocMetadata* heap_prev; // sorted by address
};

// sorted list by size
MallocMetadata dummy_free = {0, false, nullptr, nullptr, nullptr, nullptr};

MallocMetadata* heap_head;
MallocMetadata* wilderness;

bool FIRST_ALLOC = true;



/*---------------HELPER FUNCTIONS---------------------------*/

size_t align(size_t size) {
    if (size % 8 == 0) return size;
    return ((size / 8) + 1) * 8;
}

bool LARGE_ENOUGH(size_t old_size, size_t size) {
    return ( (old_size - _size_meta_data() - size) >= 128);
}

/***
 * @param block - a free block to be added to the free list
 */
void addToFreeList(MallocMetadata* block) {
    // traverse from dummy
    MallocMetadata* iter = &dummy_free;
    while (iter->next_free) {
        if (iter->next_free->size > block->size) { // find proper place
            // add
            block->prev_free = iter;
            block->next_free = iter->next_free;
            iter->next_free->prev_free = block;
            iter->next_free = block;

            return;
        }

        iter = iter->next_free;
    }

    // add at end of list
    block->next_free = nullptr;
    block->prev_free = iter;
    iter->next_free = block;
}

/***
 * @param block - an allocated block to be removed from the free list
 */
void removeFromFreeList(MallocMetadata* block) {
    block->prev_free->next_free = block->next_free;
    block->next_free->prev_free = block->prev_free;
    block->prev_free = nullptr;
    block->next_free = nullptr;
}

/***
 * @param block - a free block that is LARGE ENOUGH to be cut
 */
void cutBlocks(MallocMetadata* block, size_t wanted_size) {
    // put a new meta object in (block + _size_meta_data()  + wanted_size)
    MallocMetadata* new_block = (MallocMetadata*) ((char*)block + _size_meta_data() + wanted_size);
    new_block->size = block->size - wanted_size - _size_meta_data();
    new_block->is_free = true; // new block is free
    new_block->is_mmap = false; // new block not mmap'ed

    // add the new block to free list
    addToFreeList(new_block);

    // update old block size, remove from free list and add again (so it will be in proper place)
    block->size = wanted_size;
    removeFromFreeList(block);
    addToFreeList(block);

    // update global vars
    allocated_blocks++;                     // created new block
    allocated_bytes -= _size_meta_data();   // we've allocated this amount of bytes to be
    // metadata from the previously user bytes

    // update heap list
    new_block->heap_next = block->heap_next;
    new_block->heap_prev = block;
    block->heap_next = new_block;
}

void combineBlocks(MallocMetadata* block) {
    // 4 options:
    // prev_heap+curr+next_heap
    // curr+next_heap
    // prev_heap + curr
    // no combinations

    // if combined, remove from free list and add the new one
    // update global vars
    // update heap_list

}

/*------------ASSIGNMENT FUNCTION----------------------------------*/
void* smalloc(size_t size) {
    // if FIRST ALLOC
    if (FIRST_ALLOC) {
        void* program_break = sbrk(0);
        if (program_break == (void*)(-1)) return nullptr; // something went wrong

        // check if (sbrk(0) % 8 != 0) align (only affective for first alloc)
        if ((long)program_break % 8 != 0) {
            void* new_program_break = sbrk(8 - ((long)program_break % 8));
            if (new_program_break == (void*)(-1)) return nullptr; // something went wrong
        }
    }

    // check conditions
    if (size == 0 || size > MAX_ALLOC) return nullptr;

    // align size
    size = align(size);

    // if size >= 128*1024 use mmap (+_size_meta_data())
    if (size >= MMAP_THRESHOLD) {
        MallocMetadata* alloc = (MallocMetadata*) mmap(NULL, _size_meta_data() + size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
        alloc->size = size;
        alloc->is_mmap = true;

        // update allocated vars
        allocated_blocks++;
        allocated_bytes += size;

        return (char*)alloc + _size_meta_data();
    }

    // find the first free block that have enough size
    MallocMetadata* to_alloc = dummy_free.next_free;
    while (to_alloc) {
        if (to_alloc->size >= size) break;
        to_alloc = to_alloc->next_free;
    }
    if (to_alloc) { // we found a block!
        // if block large enough, cut it
        if (LARGE_ENOUGH(to_alloc->size, size)) {
            cutBlocks(to_alloc, size);
        }

        // mark block as alloced
        to_alloc->is_free = false;

        // update free_blocks, free_bytes
        free_blocks--;
        free_bytes -= to_alloc->size;

        // remove from list
        removeFromFreeList(to_alloc);

        // return the address after the metadata
        return (char*)to_alloc + _size_meta_data();
    }

    // if no free block was found And the wilderness chunk is free enlarge the wilderness (sbrk)
    if (wilderness->is_free) {
        size_t missing_size = size - wilderness->size; // both product of 8 so it's ok
        void* res = sbrk(missing_size);
        if (res == (void*)(-1)) return nullptr; // something went wrong

        // update global var
        allocated_bytes += missing_size;

        // update wilderness size & status
        wilderness->size += missing_size;
        wilderness->is_free = false;

        // remove wilderness from free list
        removeFromFreeList(wilderness);

        return (char*)wilderness + _size_meta_data();
    }

    // the previous program break will be the new block's place
    MallocMetadata* new_block = (MallocMetadata*) sbrk(0);
    if (new_block == (void*)(-1)) return nullptr; // somthing went wrong

    // allocate with sbrk
    void* res = sbrk(_size_meta_data() + size);
    if (res == (void*)(-1)) return nullptr; // sbrk failed

    // add metadata
    new_block->size = size;
    new_block->is_mmap = false;
    new_block->is_free = false;
    new_block->next_free = nullptr;
    new_block->prev_free = nullptr;
    new_block->heap_next = nullptr;
    new_block->heap_prev = wilderness;

    // update wilderness
    wilderness->heap_next = new_block;
    wilderness = new_block;

    // update allocated_blocks, allocated_bytes
    allocated_blocks++;
    allocated_bytes += size;

    // when return, don't forget the offset
    return (char*)new_block + _size_meta_data();

}

void* scalloc(size_t num, size_t size) {
    // use smalloc with num * size
    void* alloc = smalloc(num * size);

    // if mmaped no need to nullify
    if (((MallocMetadata*)((char*)alloc - _size_meta_data()))->is_mmap) return alloc;

    // nullify with memset
    memset(alloc, 0, num * size);

    return alloc;
}

void sfree(void* p) {
    // check if null or released
    if (!p) return;
    MallocMetadata* meta = (MallocMetadata*) ((char*)p - _size_meta_data());
    if (meta->is_free) return;

    // if block is mmap'ed than munmap
    if (meta->is_mmap) {
        // update allocated_blocks, allocated_bytes
        allocated_blocks--;
        allocated_bytes -= meta->size;

        // unmap
        assert(munmap(meta, meta->size + _size_meta_data()) == 0);

        return;
    }

    // mark as released
    meta->is_free = true;

    // add to free list
    addToFreeList(meta);

    // update used free_blocks, free_bytes
    free_blocks++;
    free_bytes += meta->size;

    // call combine
    combineBlocks(meta);
}

void* srealloc(void* oldp, size_t size) {
    // if old == null just smalloc

    // if size is smaller, reuse
    // don't update global vars

    // if mmap - unmap and than mmap
    // update allocated vars

    // try merging (prev_heap, upper_heap, three blocks)
    // if large enough call cut block
    // update heap_list
    // update free_list (you need to remove prev or next you used)
    // update free global vars

    // if i'm wilderness enlarge brk (aligned!)
    // update global vars

    // find new place with smalloc

    // copy with memcpy

    // free with sfree (only if you succeed until now)

}

size_t _num_free_blocks() {
    return free_blocks;
}

size_t _num_free_bytes() {
    return free_bytes;
}

size_t _num_allocated_blocks() {
    return allocated_blocks;
}

size_t _num_allocated_bytes() {
    return allocated_bytes;
}

size_t _num_meta_data_bytes() {
    return allocated_blocks * _size_meta_data();
}

size_t _size_meta_data() {
    return align(sizeof(MallocMetadata));
}