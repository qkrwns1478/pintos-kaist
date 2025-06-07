/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/vaddr.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);
static size_t get_free_swap_slot (void);

#define SECTOR_PER_SLOT (PGSIZE / DISK_SECTOR_SIZE)
struct bitmap *swap_slot;
/* The swap table tracks in-use and free swap slots. It should allow picking
 * an unused swap slot for evicting a page from its frame to the swap partition.
 * It should allow freeing a swap slot when its page is read back or the process
 * whose page was swapped is terminated. Swap slots should be allocated lazily, 
 * that is, only when they are actually required by eviction. 
 * Reading data pages from the executable and writing them to swap immediately at 
 * process startup is not lazy. Swap slots should not be reserved to store particular pages.
 * Free a swap slot when its contents are read back into a frame. */

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
	swap_disk = disk_get(1, 1); // Set up the swap disk
	disk_sector_t swap_disk_size = disk_size(swap_disk);
	size_t swap_slot_cnt = swap_disk_size / SECTOR_PER_SLOT;
	swap_slot = bitmap_create(swap_slot_cnt); // Data structure to manage free and used areas in the swap disk
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
	if (slot_idx == BITMAP_ERROR)
		return false;
	for (size_t i = 0; i < SECTOR_PER_SLOT; i++) {
		disk_read(swap_disk, slot_idx * SECTOR_PER_SLOT + i, kva + i * DISK_SECTOR_SIZE);
	}
	bitmap_reset(&swap_slot, slot_idx);
	anon_page->slot_idx = BITMAP_ERROR;
	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	if (page->frame == NULL)
		return false;

	size_t slot_idx = get_free_swap_slot();
	if (slot_idx == BITMAP_ERROR)
		return false;
	anon_page->slot_idx = slot_idx;

	for (size_t i = 0; i < SECTOR_PER_SLOT; i++) {
		disk_write(swap_disk, slot_idx * SECTOR_PER_SLOT + i, page->frame->kva + i * DISK_SECTOR_SIZE);
	}

	lock_acquire(&frame_table_lock);
	list_remove(&page->frame->elem);
	lock_release(&frame_table_lock);

	palloc_free_page(page->frame->kva);
	free(page->frame);
	page->frame = NULL;
	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
}

static size_t
get_free_swap_slot (void) {
	size_t slot_cnt = bitmap_size(&swap_slot);
	for (size_t i = 0; i < slot_cnt; i++) {
		if (!bitmap_test(&swap_slot, i))
			bitmap_mark(&swap_slot, i);
			return i;
	}
	return BITMAP_ERROR;
}