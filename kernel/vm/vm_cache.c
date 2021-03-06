/*
** Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
** Distributed under the terms of the NewOS License.
*/
#include <kernel/kernel.h>
#include <kernel/vm.h>
#include <kernel/vm_priv.h>
#include <kernel/vm_cache.h>
#include <kernel/vm_page.h>
#include <kernel/heap.h>
#include <kernel/int.h>
#include <kernel/khash.h>
#include <kernel/lock.h>
#include <kernel/debug.h>
#include <kernel/lock.h>
#include <kernel/smp.h>
#include <kernel/arch/cpu.h>
#include <newos/errors.h>

/* hash table of pages keyed by cache they're in and offset */
#define PAGE_TABLE_SIZE 1024 /* make this dynamic */
static void *page_cache_table;
static spinlock_t page_cache_table_lock;

struct page_lookup_key {
	off_t offset;
	vm_cache_ref *ref;
};

static int page_compare_func(void *_p, const void *_key)
{
	vm_page *p = _p;
	const struct page_lookup_key *key = _key;

//	dprintf("page_compare_func: p 0x%x, key 0x%x\n", p, key);

#if DEBUG > 1
	VERIFY_VM_PAGE(p);
#endif

	if(p->cache_ref == key->ref && p->offset == key->offset)
		return 0;
	else
		return -1;
}

#define HASH(offset, ref) ((unsigned int)(offset >> 12) ^ ((unsigned int)(ref)>>4))

static unsigned int page_hash_func(void *_p, const void *_key, unsigned int range)
{
	vm_page *p = _p;
	const struct page_lookup_key *key = _key;
#if 0
	if(p)
		dprintf("page_hash_func: p 0x%x, key 0x%x, HASH = 0x%x\n", p, key, HASH(p->offset, p->cache_ref) % range);
	else
		dprintf("page_hash_func: p 0x%x, key 0x%x, HASH = 0x%x\n", p, key, HASH(key->offset, key->ref) % range);
#endif

#if DEBUG > 1
	if(p != NULL)
		VERIFY_VM_PAGE(p);
#endif

	if(p)
		return HASH(p->offset, p->cache_ref) % range;
	else
		return HASH(key->offset, key->ref) % range;
}

int vm_cache_init(kernel_args *ka)
{
	page_cache_table = hash_init(PAGE_TABLE_SIZE,
		offsetof(vm_page, hash_next),
		&page_compare_func,
		&page_hash_func);
	if(!page_cache_table)
		panic("vm_cache_init: cannot allocate memory for page cache hash table\n");
	page_cache_table_lock = 0;

	return 0;
}

vm_cache *vm_cache_create(vm_store *store)
{
	vm_cache *cache;

	cache = kmalloc(sizeof(vm_cache));
	if(cache == NULL)
		return NULL;

	cache->magic = VM_CACHE_MAGIC;
	list_initialize(&cache->page_list_head);
	cache->ref = NULL;
	cache->source = NULL;
	cache->store = store;
	if(store != NULL)
		store->cache = cache;
	cache->virtual_size = 0;
	cache->temporary = 0;
	cache->scan_skip = 0;

	return cache;
}

vm_cache_ref *vm_cache_ref_create(vm_cache *cache)
{
	vm_cache_ref *ref;

	ref = kmalloc(sizeof(vm_cache_ref));
	if(ref == NULL)
		return NULL;

	ref->magic = VM_CACHE_REF_MAGIC;
	ref->cache = cache;
	mutex_init(&ref->lock, "cache_ref_mutex");
	list_initialize(&ref->region_list_head);
	ref->ref_count = 0;
	cache->ref = ref;

	return ref;
}

void vm_cache_acquire_ref(vm_cache_ref *cache_ref, bool acquire_store_ref)
{
//	dprintf("vm_cache_acquire_ref: cache_ref 0x%x, ref will be %d\n", cache_ref, cache_ref->ref_count+1);

	if(cache_ref == NULL)
		panic("vm_cache_acquire_ref: passed NULL\n");
	VERIFY_VM_CACHE_REF(cache_ref);
	VERIFY_VM_CACHE(cache_ref->cache);
	VERIFY_VM_STORE(cache_ref->cache->store);

	if(acquire_store_ref && cache_ref->cache->store->ops->acquire_ref) {
		cache_ref->cache->store->ops->acquire_ref(cache_ref->cache->store);
	}
	atomic_add(&cache_ref->ref_count, 1);
}

void vm_cache_release_ref(vm_cache_ref *cache_ref)
{
//	dprintf("vm_cache_release_ref: cache_ref 0x%x, ref will be %d\n", cache_ref, cache_ref->ref_count-1);

	if(cache_ref == NULL)
		panic("vm_cache_release_ref: passed NULL\n");
	VERIFY_VM_CACHE_REF(cache_ref);
	VERIFY_VM_CACHE(cache_ref->cache);

	if(atomic_add(&cache_ref->ref_count, -1) == 1) {
		// delete this cache
		// delete the cache's backing store, if it has one
		off_t store_committed_size = 0;
		if(cache_ref->cache->store) {
			VERIFY_VM_STORE(cache_ref->cache->store);
			store_committed_size = cache_ref->cache->store->committed_size;
			(*cache_ref->cache->store->ops->destroy)(cache_ref->cache->store);
		}

		// free all of the pages in the cache
		vm_page *last, *next;
		last = NULL;
		list_for_every_entry_safe(&cache_ref->cache->page_list_head, last, next, vm_page, cache_node) {
			VERIFY_VM_PAGE(last);

			// remove it from the cache list
			list_delete(&last->cache_node);

			// remove it from the hash table
			int_disable_interrupts();
			acquire_spinlock(&page_cache_table_lock);

			hash_remove(page_cache_table, last);

			release_spinlock(&page_cache_table_lock);
			int_restore_interrupts();

//			dprintf("vm_cache_release_ref: freeing page 0x%x\n", old_page->ppn);
			vm_page_set_state(last, PAGE_STATE_FREE);
		}
		vm_increase_max_commit(cache_ref->cache->virtual_size - store_committed_size);

		// remove the ref to the source
		if(cache_ref->cache->source)
			vm_cache_release_ref(cache_ref->cache->source->ref);

		mutex_destroy(&cache_ref->lock);
		kfree(cache_ref->cache);
		kfree(cache_ref);

		return;
	}
	if(cache_ref->cache->store->ops->release_ref) {
		cache_ref->cache->store->ops->release_ref(cache_ref->cache->store);
	}
}

vm_page *vm_cache_lookup_page(vm_cache_ref *cache_ref, off_t offset)
{
	vm_page *page;
	struct page_lookup_key key;

	VERIFY_VM_CACHE_REF(cache_ref);

	key.offset = offset;
	key.ref = cache_ref;

	int_disable_interrupts();
	acquire_spinlock(&page_cache_table_lock);

	page = hash_lookup(page_cache_table, &key);

	release_spinlock(&page_cache_table_lock);
	int_restore_interrupts();

	return page;
}

void vm_cache_insert_page(vm_cache_ref *cache_ref, vm_page *page, off_t offset)
{

//	dprintf("vm_cache_insert_page: cache 0x%x, page 0x%x, offset 0x%x 0x%x\n", cache_ref, page, offset);

	VERIFY_VM_CACHE_REF(cache_ref);
	VERIFY_VM_CACHE(cache_ref->cache);
	VERIFY_VM_PAGE(page);

	page->offset = offset;

	list_add_head(&cache_ref->cache->page_list_head, &page->cache_node);
	page->cache_ref = cache_ref;

	int_disable_interrupts();
	acquire_spinlock(&page_cache_table_lock);

	hash_insert(page_cache_table, page);

	release_spinlock(&page_cache_table_lock);
	int_restore_interrupts();

}

void vm_cache_remove_page(vm_cache_ref *cache_ref, vm_page *page)
{

//	dprintf("vm_cache_remove_page: cache 0x%x, page 0x%x\n", cache_ref, page);

	VERIFY_VM_CACHE_REF(cache_ref);
	VERIFY_VM_CACHE(cache_ref->cache);
	VERIFY_VM_PAGE(page);

	int_disable_interrupts();
	acquire_spinlock(&page_cache_table_lock);

	hash_remove(page_cache_table, page);

	release_spinlock(&page_cache_table_lock);
	int_restore_interrupts();

	list_delete(&page->cache_node);
	page->cache_ref = NULL;
}

int vm_cache_insert_region(vm_cache_ref *cache_ref, vm_region *region)
{
	mutex_lock(&cache_ref->lock);

	VERIFY_VM_CACHE_REF(cache_ref);
	VERIFY_VM_REGION(region);

	list_add_head(&cache_ref->region_list_head, &region->cache_node);

	mutex_unlock(&cache_ref->lock);
	return 0;
}

int vm_cache_remove_region(vm_cache_ref *cache_ref, vm_region *region)
{
	mutex_lock(&cache_ref->lock);

	VERIFY_VM_CACHE_REF(cache_ref);
	VERIFY_VM_REGION(region);

	list_delete(&region->cache_node);

	mutex_unlock(&cache_ref->lock);
	return 0;
}
