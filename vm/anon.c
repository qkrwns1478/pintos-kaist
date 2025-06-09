/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/vaddr.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

#define SECTOR_PER_SLOT (PGSIZE / DISK_SECTOR_SIZE)
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
	disk_sector_t swap_disk_size = disk_size(swap_disk);
	size_t swap_slot_cnt = swap_disk_size / SECTOR_PER_SLOT;
	swap_slot = bitmap_create(swap_slot_cnt);
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
	size_t idx = page->anon.slot_idx;

	if (idx == BITMAP_ERROR || !bitmap_test(swap_slot, idx))
		return false;

	for (size_t i = 0; i < SECTOR_PER_SLOT; i++)
		disk_read(swap_disk, idx * SECTOR_PER_SLOT + i, kva + i * DISK_SECTOR_SIZE);

	bitmap_reset(swap_slot, idx);
	page->anon.slot_idx = BITMAP_ERROR;

	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	if (!page->frame)
		return false;

	size_t idx = bitmap_scan_and_flip(swap_slot, 0, 1, false);
	if (idx == BITMAP_ERROR)
		return false;

	page->anon.slot_idx = idx;

	for (size_t i = 0; i < SECTOR_PER_SLOT; i++)
		disk_write(swap_disk, idx * SECTOR_PER_SLOT + i, page->frame->kva + i * DISK_SECTOR_SIZE);

	list_remove(&page->frame->frame_elem);
	palloc_free_page(page->frame->kva);
	free(page->frame);
	page->frame = NULL;

	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	if (page->frame) {
		list_remove(&page->frame->frame_elem);
		palloc_free_page(page->frame->kva);
		free(page->frame);
		page->frame = NULL;
	}

	if (page->anon.slot_idx != BITMAP_ERROR) {
		bitmap_reset(swap_slot, page->anon.slot_idx);
		page->anon.slot_idx = BITMAP_ERROR;
	}
}