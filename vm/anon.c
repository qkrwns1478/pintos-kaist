/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "devices/disk.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);
static size_t get_free_swap_slot (void);

#define SWAP_SLOTS_CNT (PGSIZE / DISK_SECTOR_SIZE)
struct bitmap *swap_slot;

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1, 1);
	size_t swap_slot_cnt = disk_size(swap_disk) / SWAP_SLOTS_CNT;
	swap_slot = bitmap_create(swap_slot_cnt);
	ASSERT(swap_slot != NULL);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;
	anon_page->slot_idx = BITMAP_ERROR;
	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	size_t slot_idx = anon_page->slot_idx;
	if (slot_idx == BITMAP_ERROR) {
		// 새로 만든 스택 페이지: swap에서 불러올 게 없음
        memset(kva, 0, PGSIZE);
        return true;
	}
    for (int i = 0; i < SWAP_SLOTS_CNT; i++) {
        disk_read(swap_disk, slot_idx * SWAP_SLOTS_CNT + i, kva + i * DISK_SECTOR_SIZE);
    }
    bitmap_reset(swap_slot, slot_idx);
	anon_page->slot_idx = BITMAP_ERROR;
    return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	size_t slot_idx = bitmap_scan(swap_slot, 0, 1, false);
	if (slot_idx == BITMAP_ERROR)
        return false;
	bitmap_mark(swap_slot, slot_idx);

    anon_page->slot_idx = slot_idx;
    for (int i = 0; i < SWAP_SLOTS_CNT; i++) {
        disk_write(swap_disk, slot_idx * SWAP_SLOTS_CNT + i, page->frame->kva + i * DISK_SECTOR_SIZE);
    }
	pml4_clear_page(thread_current()->pml4, page->va);
	list_remove(&page->frame->frame_elem);
    page->frame = NULL;
    return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	// if (anon_page->slot_idx != BITMAP_ERROR) {
	// 	bitmap_reset(swap_slot, anon_page->slot_idx);
	// 	anon_page->slot_idx = BITMAP_ERROR;
	// }
}
