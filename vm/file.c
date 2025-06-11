/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "userprog/process.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "filesys/filesys.h"

#define MIN(x, y) ((x) < (y) ? (x) : (y))

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;
	struct file_page *file_page = &page->file;
	file_page->va = page->va;
	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
	// lock_acquire(&filesys_lock);
	struct file *file = file_page->file;
    off_t ofs = file_page->ofs;
    size_t read_bytes = file_page->read_bytes;
    size_t zero_bytes = file_page->zero_bytes;
    if (file_read_at(file, kva, read_bytes, ofs) != read_bytes) {
		lock_release(&filesys_lock);
		return false;
	}
	// lock_release(&filesys_lock);
    memset(kva + read_bytes, 0, zero_bytes);
    return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;

	if (pml4_is_dirty(thread_current()->pml4,page->va)){
		// lock_acquire(&filesys_lock);
		file_write_at(file_page->file, page->frame->kva, file_page->read_bytes, file_page->ofs);
		pml4_set_dirty(thread_current()->pml4, page->va, false);
		// lock_release(&filesys_lock);
	}
	page->frame->page = NULL;
    page->frame = NULL;
	pml4_clear_page(thread_current()->pml4, page->va);
	// list_remove(&page->frame->frame_elem);
	// // palloc_free_page(page->frame->kva);
	// // free(page->frame);
	// page->frame = NULL;
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	if (file_page->file == NULL)
		return;
    if (pml4_is_dirty(thread_current()->pml4, page->va)) {
        file_write_at(file_page->file, page->frame->kva, file_page->read_bytes, file_page->ofs);
        pml4_set_dirty(thread_current()->pml4, page->va, false);
    }
    pml4_clear_page(thread_current()->pml4, page->va);
	if (page->frame != NULL) {
		list_remove(&page->frame->frame_elem);
		palloc_free_page(page->frame->kva);
		free(page->frame);
		page->frame = NULL;
	}
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset) {
	lock_acquire(&filesys_lock);
	struct file *file_ = file_reopen(file);
	if (file_ == NULL) {
		lock_release(&filesys_lock);
		return NULL;
	}
	size_t read_bytes = MIN(length, file_length(file_) - offset);
	size_t zero_bytes = pg_round_up(length) - read_bytes;

	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (addr) == 0);
	ASSERT (offset % PGSIZE == 0);

	void *ret = addr;
    while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = MIN(read_bytes, PGSIZE);
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        struct lazy_load_args *aux = (struct lazy_load_args *)malloc(sizeof(struct lazy_load_args));
		if (aux == NULL) {
			lock_release(&filesys_lock);
			return NULL;
		}
        aux->file = file_;
        aux->ofs = offset;
        aux->read_bytes = page_read_bytes;
        aux->zero_bytes = page_zero_bytes;

        if (!vm_alloc_page_with_initializer(VM_FILE, addr, writable, lazy_load_segment, aux)) {
			lock_release(&filesys_lock);
			free(aux);
			return NULL;
		}

        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        addr += PGSIZE;
        offset += page_read_bytes;
    }
	lock_release(&filesys_lock);
    return ret;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	/* TODO: Remove mmaped page from mmap list of current thread */
	struct thread *curr = thread_current();
	struct page *page;
	lock_acquire(&filesys_lock);
    while ((page = spt_find_page(&curr->spt, addr))) {
		destroy(page);
		page->file.file = NULL;
		// list_remove(&page->file.elem);
		// list_remove(&page->frame->frame_elem);
		spt_remove_page(&curr->spt, page);
		addr += PGSIZE;
	}
	lock_release(&filesys_lock);
}
