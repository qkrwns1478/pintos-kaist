/* vm.c: Generic interface for virtual memory objects. */
#include <hash.h>
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "userprog/process.h"
#include "vm/vm.h"
#include "vm/inspect.h"

struct list frame_table;
struct list_elem *fte;
static struct lock frame_table_lock;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	list_init(&frame_table);
	lock_init(&frame_table_lock);
	lock_init(&hash_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

struct hash pages;

unsigned page_hash (const struct hash_elem *p_, void *aux UNUSED);
bool page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED);
struct page *page_lookup (const void *address);

/* Returns a hash value for page p. */
unsigned
page_hash (const struct hash_elem *p_, void *aux UNUSED) {
	const struct page *p = hash_entry (p_, struct page, hash_elem);
	return hash_bytes (&p->va, sizeof p->va);
}

bool
page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) {
	const struct page *a = hash_entry (a_, struct page, hash_elem);
	const struct page *b = hash_entry (b_, struct page, hash_elem);
	return a->va < b->va;
}

/* Returns the page containing the given virtual address, or a null pointer if no such page exists. */
struct page *
page_lookup (const void *address) {
	struct page p;
	struct hash_elem *e;

	p.va = address;
	e = hash_find (&pages, &p.hash_elem);
	return e != NULL ? hash_entry (e, struct page, hash_elem) : NULL;
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);
static void spt_kill_destructor (struct hash_elem *h, void *aux UNUSED);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable, vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;
	if (!is_user_vaddr(upage) || upage == NULL)
		return false;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		struct page *p = (struct page *)malloc(sizeof(struct page)); // page structure → using malloc
		if (VM_TYPE(type) == VM_ANON)
			uninit_new(p, upage, init, type, aux, anon_initializer);
		else if (VM_TYPE(type) == VM_FILE)
			uninit_new(p, upage, init, type, aux, file_backed_initializer);
		
		/* Set writable bit */
		p->writable = writable;

		/* TODO: Insert the page into the spt. */
		if (!spt_insert_page(spt, p)) {
			free(p);
			goto err;
		}
		return true;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	/* TODO: Fill this function. */
	if(spt == NULL || va == NULL) return NULL;
	struct page page;
	page.va = pg_round_down(va);
    struct hash_elem *e = hash_find(&spt->spt_hash, &page.hash_elem);
	if (e != NULL) return hash_entry(e, struct page, hash_elem);
	else return NULL;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED, struct page *page UNUSED) {
	bool succ = false;
	/* TODO: Fill this function. */
	lock_acquire(&hash_lock);
	if (hash_insert(&spt->spt_hash, &page->hash_elem) == NULL)
		succ = true;
	lock_release(&hash_lock);
	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	hash_delete(&thread_current()->spt.spt_hash, &page->hash_elem);
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	// lock_acquire(&frame_table_lock);
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */
	if (list_empty(&frame_table)) {
		// lock_release(&frame_table_lock);
		return NULL;
	}

	if (fte == NULL || fte == list_end(&frame_table))
		fte = list_begin(&frame_table);

	while (true) {
		struct frame *frame = list_entry(fte, struct frame, frame_elem);
		struct page *page = frame->page;

		if (!pml4_is_accessed(thread_current()->pml4, page->va)) {
			victim = frame;
			break;
		} else {
			pml4_set_accessed(thread_current()->pml4, page->va, false);
			if (fte == list_end(&frame_table)) fte = list_begin(&frame_table);
			else fte = list_next(fte);
		}
	}
	// lock_release(&frame_table_lock);
	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	lock_acquire(&frame_table_lock);
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */
	if (victim == NULL)
		goto err;

	struct page *page = victim->page;

	if (!swap_out(page))
		goto err;

	pml4_clear_page(thread_current()->pml4, page->va);

	victim->page = NULL;
	page->frame = NULL;

	list_remove(&victim->frame_elem);
	lock_release(&frame_table_lock);

	return victim;
err:
	lock_release(&frame_table_lock);
	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */
	void *kva = palloc_get_page(PAL_USER | PAL_ZERO);
    if (kva == NULL) {
        // 메모리 부족 → evict 필요
		struct frame *victim = vm_evict_frame();
		if (victim == NULL)
			PANIC("No frame to evict!");

		kva = victim->kva;
		free(victim);  // 회수한 frame 구조체 메모리 해제
	}
	
	frame = (struct frame *)malloc(sizeof(struct frame));
	frame->kva = kva;
	frame->page = NULL;

	lock_acquire(&frame_table_lock);
	list_push_back(&frame_table, &frame->frame_elem);  // frame table 등록
	lock_release(&frame_table_lock);

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static bool
vm_stack_growth (void *addr UNUSED) {
	/* TODO: To implement stack growth functionalities, you first modify 
	 * TODO: vm_try_handle_fault() to identify the stack growth.  
	 * TODO: After identifying the stack growth, you should make a call 
	 * TODO: to vm_stack_growth() to grow  the stack. */
	/* Increases the stack size by allocating one or more anonymous pages
	 * so that addr is no longer a faulted address. Make sure you round down
	 * the addr to PGSIZE when handling the allocation. */
	void *upage = pg_round_down(addr);
	int cnt = 0;
	while (!spt_find_page(&thread_current()->spt, upage + cnt * PGSIZE))
		cnt++;
	for (int i = 0; i < cnt; i++) {
		if (!vm_alloc_page_with_initializer(VM_ANON | VM_MARKER_0, upage + i * PGSIZE, true, NULL, NULL))
			return false;
		if (!vm_claim_page(upage + i * PGSIZE))
			return false;
	}
	return true;
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED, bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	/* Modify this function to resolve the page struct corresponding to the faulted address
	 * by consulting to the supplemental page table through spt_find_page. */
    if (addr == NULL || is_kernel_vaddr(addr) || !not_present)
		return false;
	struct page *page = spt_find_page(spt, addr);
    if (page == NULL) {
		/* If you have confirmed that the fault can be handled with a stack growth,
		 * call vm_stack_growth with the faulted address. */
		void *rsp = thread_current()->stack_pointer;
		if (rsp - PGSIZE < addr && addr < USER_STACK && rsp - PGSIZE >= STACK_LIMIT) {
			if (!vm_stack_growth(addr))
				return false;
			page = spt_find_page(spt, addr);
		} else {
			// printf("[vm_try_handle_fault] stack growth failed: %d %d %d\n", rsp - PGSIZE < addr, addr < USER_STACK, rsp - PGSIZE >= STACK_LIMIT);
			// if (rsp - PGSIZE >= addr) printf("[vm_try_handle_fault] rsp = %p, addr = %p\n", rsp, addr);
			return false;
		}
	}
	if (write && !page->writable)
		return false;
    return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	/* TODO: Fill this function */
	if (va == NULL)
		return false;
	struct page *page = spt_find_page(&thread_current()->spt, va);
	if (page == NULL)
		return false;
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();
	if (frame == NULL)
		return false;

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	/* Add the mapping from the VA to the PA in the page table. */
	if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable))
		return false;

	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init (&spt->spt_hash, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst, struct supplemental_page_table *src) {
    struct hash_iterator i;
    hash_first(&i, &src->spt_hash);
    while (hash_next(&i)) {
        struct page *src_page = hash_entry(hash_cur(&i), struct page, hash_elem);
        enum vm_type type = src_page->operations->type;
        void *va = src_page->va;
        bool writable = src_page->writable;
        if (type == VM_UNINIT) {
            // vm_alloc_page_with_initializer(src_page->uninit.type, va, writable, src_page->uninit.init, src_page->uninit.aux);
			if (!vm_alloc_page_with_initializer(VM_ANON, va, writable, src_page->uninit.init, src_page->uninit.aux))
                return false;
            continue;
		} else if (type == VM_FILE) {
			struct lazy_load_args *lla = (struct lazy_load_args *)malloc(sizeof(struct lazy_load_args));
			lla->file = src_page->file.file;
			lla->ofs = src_page->file.ofs;
			lla->read_bytes = src_page->file.read_bytes;
			if (!vm_alloc_page_with_initializer(type, va, writable, NULL, lla))
				return false;
			struct page *file_page = spt_find_page(dst, va);
			file_backed_initializer(file_page, type, NULL);
			pml4_set_page(thread_current()->pml4, file_page->va, src_page->frame->kva, src_page->writable);
			continue;
		}
		if (!vm_alloc_page_with_initializer(type, va, writable, NULL, NULL) || !vm_claim_page(va))
            return false;
        struct page *dst_page = spt_find_page(dst, va);
        memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
    }
    return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	lock_acquire(&hash_lock);
	hash_clear(&spt->spt_hash, spt_kill_destructor);
	lock_release(&hash_lock);
}

static void spt_kill_destructor (struct hash_elem *h, void *aux UNUSED) {
	struct page *page = hash_entry(h, struct page, hash_elem);
	destroy(page);
	free(page);
}