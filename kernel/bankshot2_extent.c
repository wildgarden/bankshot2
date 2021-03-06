#include "bankshot2.h"


/* ========================== Extent Tree ============================= */

static inline int bankshot2_rbtree_compare(struct extent_entry *curr,
		struct extent_entry *new)
{
	if (new->offset < curr->offset) return -1;
	if (new->offset > curr->offset) return 1;

	return 0;
}

static inline int bankshot2_rbtree_compare_find(struct extent_entry *curr,
		off_t offset)
{
	if ((curr->offset <= offset) &&
			(curr->offset + curr->length > offset))
		return 0;

	if (offset < curr->offset) return -1;
	if (offset > curr->offset) return 1;

	return 0;
}

void bankshot2_free_extent(struct bankshot2_device *bs2_dev,
		struct extent_entry *extent)
{
	struct vma_list *next, *delete;

	list_for_each_entry_safe(delete, next, &extent->vma_list, list) {
		list_del(&delete->list);
		kfree(delete);
	}

	kmem_cache_free(bs2_dev->bs2_extent_slab, extent);
}

struct extent_entry * bankshot2_find_extent(struct bankshot2_device *bs2_dev,
		struct bankshot2_inode *pi, off_t offset)
{
	struct extent_entry *curr;
	struct rb_node *temp;
	int compVal;

//	read_lock(&pi->extent_tree_lock);
	temp = pi->extent_tree.rb_node;
	while (temp) {
		curr = container_of(temp, struct extent_entry, node);
		compVal = bankshot2_rbtree_compare_find(curr, offset);

		if (compVal == -1) {
			temp = temp->rb_left;
		} else if (compVal == 1) {
			temp = temp->rb_right;
		} else {
//			extent->offset = curr->offset;
//			extent->length = curr->length;
//			extent->dirty = curr->dirty;
//			extent->mmap_addr = curr->mmap_addr;
//			read_unlock(&pi->extent_tree_lock);

			return curr;
		}
	}

//	read_unlock(&pi->extent_tree_lock);
	return NULL;
}

void bankshot2_clear_extent_access(struct bankshot2_device *bs2_dev,
		struct bankshot2_inode *pi, unsigned long index)
{
	struct extent_entry *curr;
	off_t offset;
	struct rb_node *temp;
	int compVal;

	offset = index << bs2_dev->s_blocksize_bits;

//	read_lock(&pi->extent_tree_lock);
	temp = pi->extent_tree.rb_node;
	while (temp) {
		curr = container_of(temp, struct extent_entry, node);
		compVal = bankshot2_rbtree_compare_find(curr, offset);

		if (compVal == -1) {
			temp = temp->rb_left;
		} else if (compVal == 1) {
			temp = temp->rb_right;
		} else {
			bs2_dbg("Clear pi %llu, extent offset 0x%lx access\n",
				pi->i_ino, curr->offset);
			atomic_set(&curr->access, 0);
//			read_unlock(&pi->extent_tree_lock);
			return;
		}
	}

//	read_unlock(&pi->extent_tree_lock);
	return;
}

void bankshot2_remove_extent(struct bankshot2_device *bs2_dev,
			struct bankshot2_inode *pi, off_t offset)
{
	struct extent_entry *curr;
	struct rb_node *temp;
	int compVal;

//	write_lock(&pi->extent_tree_lock);
	temp = pi->extent_tree.rb_node;
	while (temp) {
		curr = container_of(temp, struct extent_entry, node);
		compVal = bankshot2_rbtree_compare_find(curr, offset);

		if (compVal == -1) {
			temp = temp->rb_left;
		} else if (compVal == 1) {
			temp = temp->rb_right;
		} else {
			bs2_dbg("Delete extent to pi %llu, extent offset %lu, "
				"length %lu\n",
				pi->i_ino, curr->offset, curr->length);
			rb_erase(&curr->node, &pi->extent_tree);
			pi->num_extents--;
			bankshot2_free_extent(bs2_dev, curr);
			break;
		}
	}

//	write_unlock(&pi->extent_tree_lock);
	return;
}

/* Use an list to store the vmas */
static void bankshot2_insert_vma(struct bankshot2_device *bs2_dev,
		struct extent_entry *extent, struct vm_area_struct *vma)
{
	struct vma_list *inserted_vma, *new_vma;

	list_for_each_entry(inserted_vma, &extent->vma_list, list) {
		if (inserted_vma->vma == vma)
			return;
	}

	new_vma = kzalloc(sizeof(struct vma_list), GFP_ATOMIC);
	BUG_ON(!new_vma);

	new_vma->vma = vma;
	INIT_LIST_HEAD(&new_vma->list);
	list_add_tail(&new_vma->list, &extent->vma_list);
#if 0
	bs2_info("Extent offset 0x%lx, length 0x%lx VMAs:\n", extent->offset, extent->length);
	list_for_each_entry(temp, &extent->vma_list, list) {
		bs2_info("Vma: start %lx, pgoff %lx, end %lx, mm %p\n",
		temp->vma->vm_start, temp->vma->vm_pgoff, temp->vma->vm_end, temp->vma->vm_mm);
	}
#endif
	return;
}

static void bankshot2_initialize_new_extent(struct bankshot2_device *bs2_dev,
		struct extent_entry *new, off_t offset, size_t length,
		unsigned long b_offset, struct address_space *mapping,
		struct vm_area_struct *vma)
{
	new->offset = offset;
	new->length = length;
	new->b_offset = b_offset;
	new->dirty = 1; //FIXME: assume all extents are dirty
	new->mapping = mapping;

	INIT_LIST_HEAD(&new->vma_list);
	bankshot2_insert_vma(bs2_dev, new, vma);
}

int bankshot2_add_extent(struct bankshot2_device *bs2_dev,
		struct bankshot2_inode *pi, off_t offset, size_t length,
		unsigned long b_offset, struct address_space *mapping,
		struct vm_area_struct *vma,
		struct extent_entry **access_extent)
{
	struct extent_entry *curr, *new, *next;
	struct rb_node **temp, *parent, *next_node;
	off_t extent_offset;
	size_t extent_length;
	unsigned long extent_b_offset;
	int compVal;
	int no_new = 0;

	bs2_dbg("Insert extent to pi %llu, extent offset %lx, "
			"length %lu,  b_offset %lx\n",
			pi->i_ino, offset, length, b_offset);

	/* Break the extent to PAGE_SIZE chunks */
	if (offset != ALIGN_DOWN(offset) || length != ALIGN_DOWN(length)) {
		bs2_info("%s: inode %llu: offset or length not aligned to mmap "
				"unit size! offset 0x%lx, length %lu\n",
				__func__, pi->i_ino, offset, length);
		return 0;
	}

//	write_lock(&pi->extent_tree_lock);
	temp = &(pi->extent_tree.rb_node);
	parent = NULL;

	extent_offset = offset;
	extent_b_offset = b_offset;
	extent_length = length;

	while (*temp) {
		curr = container_of(*temp, struct extent_entry, node);
		compVal = bankshot2_rbtree_compare_find(curr,
					extent_offset);
		parent = *temp;

		if (compVal == -1) {
			temp = &((*temp)->rb_left);
		} else if (compVal == 1) {
			temp = &((*temp)->rb_right);
		} else {
			if (curr->offset != extent_offset
					|| curr->length > extent_length
					|| curr->b_offset != extent_b_offset
					|| curr->mapping != mapping) {
				bs2_info("Existing extent hit but unmatch! "
					"existing extent offset 0x%lx, "
					"length %lu, b_offset 0x%lx, "
					"mapping %p, "
					"new extent offset 0x%lx, length %lu, "
					"b_offset 0x%lx, mapping %p\n",
					curr->offset, curr->length,
					curr->b_offset, curr->mapping,
					extent_offset, extent_length,
					extent_b_offset, mapping);

				no_new = 1;
				break;
			}
			bankshot2_insert_vma(bs2_dev, curr, vma);
			if (curr->length < extent_length) {
				curr->length = extent_length;
				atomic_set(&curr->access, 1);
				new = curr;
				goto check_overlap;
			}

			no_new = 1;
			break;
		}
	}

	if (no_new) {
		bs2_dbg("Set pi %llu, extent offset 0x%lx access\n",
			pi->i_ino, curr->offset);
		atomic_set(&curr->access, 1);
		return 0;
	}

	new = (struct extent_entry *)
		kmem_cache_alloc(bs2_dev->bs2_extent_slab, GFP_KERNEL);
	if (!new) {
//		write_unlock(&pi->extent_tree_lock);
		return -ENOMEM;
	}

	bankshot2_initialize_new_extent(bs2_dev, new, extent_offset,
		extent_length, extent_b_offset, mapping, vma);

	atomic_set(&new->access, 1);
	*access_extent = new;
	bs2_dbg("Set pi %llu, extent offset 0x%lx access\n",
			pi->i_ino, new->offset);
	rb_link_node(&new->node, parent, temp);
	rb_insert_color(&new->node, &pi->extent_tree);
	pi->num_extents++;

check_overlap:
	// Check the next node see if it overlaps
	next_node = rb_next(&new->node);
	if (!next_node)
		return 0;

	next = container_of(next_node, struct extent_entry, node);
	if (new->offset + new->length > next->offset)
		new->length = next->offset - new->offset;

	return 0;
}

unsigned long bankshot2_get_dirty_page_array(struct bankshot2_device *bs2_dev,
		struct bankshot2_inode *pi, struct extent_entry *extent,
		char *void_array, size_t count)
{
	unsigned long required = 0;
	struct vma_list *temp;
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	unsigned long address;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	int i;

	list_for_each_entry(temp, &extent->vma_list, list) {
		vma = temp->vma;
		mm = vma->vm_mm;
		address = vma->vm_start;

		spin_lock(&mm->page_table_lock);
		for (i = 0; i < count; i++, address += PAGE_SIZE) {
			if (void_array[i] == 0x1)
				continue;

			if (address < vma->vm_start || address >= vma->vm_end) {
				bs2_info("%s: address not in vma\n", __func__);
				continue;
			}

			pgd = pgd_offset(mm, address);
			if (!pgd_present(*pgd)) {
				bs2_info("%s: pgd not found\n", __func__);
				continue;
			}

			pud = pud_offset(pgd, address);
			if (!pud_present(*pud)) {
				bs2_info("%s: pud not found\n", __func__);
				continue;
			}

			pmd = pmd_offset(pud, address);
			if (!pmd_present(*pmd)) {
				bs2_info("%s: pmd not found\n", __func__);
				continue;
			}

			pte = pte_offset_map(pmd, address);
			if (!pte_present(*pte)) {
				bs2_info("%s: pte not found\n", __func__);
				continue;
			}

			if (pte_dirty(*pte)) {
				void_array[i] = 0x1;
				required++;
			}
		}
		spin_unlock(&mm->page_table_lock);
	}

	return required;
}

void bankshot2_print_tree(struct bankshot2_device *bs2_dev,
				struct bankshot2_inode *pi)
{
	struct extent_entry *curr;
	struct rb_node *temp;

//	read_lock(&pi->extent_tree_lock);
	temp = rb_first(&pi->extent_tree);
	bs2_info("Print extent tree for pi %llu\n", pi->i_ino);
	while (temp) {
		curr = container_of(temp, struct extent_entry, node);
		bs2_info("pi %llu, extent offset %lu, length %lu\n",
				pi->i_ino, curr->offset, curr->length);
		temp = rb_next(temp);
	}

//	read_unlock(&pi->extent_tree_lock);
	return;
}

void bankshot2_delete_tree(struct bankshot2_device *bs2_dev,
				struct bankshot2_inode *pi)
{
	struct extent_entry *curr;
	struct rb_node *temp;

//	write_lock(&pi->extent_tree_lock);
	temp = rb_first(&pi->extent_tree);
	while (temp) {
		curr = container_of(temp, struct extent_entry, node);
//		bs2_info("pi %llu, extent offset %lu, length %lu, "
//				"mmap addr %lx\n", pi->i_ino, curr->offset,
//				curr->length, curr->mmap_addr);
		temp = rb_next(temp);
		rb_erase(&curr->node, &pi->extent_tree);
		bankshot2_free_extent(bs2_dev, curr);
	}

//	write_unlock(&pi->extent_tree_lock);
	pi->num_extents = 0;
	return;
}

static struct extent_entry* bankshot2_get_victim_extent(
		struct bankshot2_device *bs2_dev, struct bankshot2_inode *pi)
{
	struct extent_entry *victim;
	struct rb_node *temp;

	temp = rb_first(&pi->extent_tree);

	while (temp) {
		victim = container_of(temp, struct extent_entry, node);
//		bs2_info("pi %llu, extent offset %lu, length %lu, "
//				"mmap addr %lx\n", pi->i_ino, curr->offset,
//				curr->length, curr->mmap_addr);
		if (atomic_read(&victim->access) == 0)
			goto found;		
		temp = rb_next(temp);
	}

	return NULL;

found:
	rb_erase(&victim->node, &pi->extent_tree);
	pi->num_extents--;

	return victim;
}

int bankshot2_evict_extent(struct bankshot2_device *bs2_dev,
		struct bankshot2_inode *pi, struct bankshot2_cache_data *data,
		int *num_free)
{
	struct extent_entry *victim;
	int ret = 0;
	u64 block;
	unsigned long pfn;

//	bs2_info("Before free:\n");
//	bankshot2_print_tree(bs2_dev, pi);

//	write_lock(&pi->extent_tree_lock);
	victim = bankshot2_get_victim_extent(bs2_dev, pi);
//	write_unlock(&pi->extent_tree_lock);

	if (!victim)
		return -ENOMEM;

	bs2_info("%s: pi %llu, extent offset 0x%lx, length 0x%lx\n",
		__func__, pi->i_ino, victim->offset, victim->length);

//	bankshot2_munmap_extent(bs2_dev, pi, victim);

//	if (victim->dirty)
	ret = bankshot2_write_back_extent(bs2_dev, pi, data, victim);

	*num_free = victim->length >> PAGE_SHIFT;
	block = bankshot2_find_data_block(bs2_dev, pi,
					victim->offset >> PAGE_SHIFT);
	pfn = bankshot2_get_pfn(bs2_dev, block);
	bs2_dbg("Free pfn @ 0x%lx, file offset 0x%lx\n", pfn, victim->offset);

	bankshot2_truncate_blocks(bs2_dev, pi, victim->offset,
					victim->offset + victim->length);

	bankshot2_free_extent(bs2_dev, victim);

//	bs2_info("After free:\n");
//	bankshot2_print_tree(bs2_dev, pi);
	return ret;
}

static void
bankshot2_remove_mapping_from_extent(struct bankshot2_device *bs2_dev,
		struct extent_entry *extent, struct mm_struct *mm)
{
	struct vma_list *delete, *next;
	struct vm_area_struct *vma;
	unsigned long address;
	unsigned long pgoff = 0;
//	char *void_array;
//	size_t count;
//	unsigned long required;

	pgoff = extent->offset >> PAGE_SHIFT;

	list_for_each_entry_safe(delete, next, &extent->vma_list, list) {
		if (delete->vma->vm_mm == mm) {
			vma = delete->vma;
			bs2_dbg("remove vma %p: start 0x%lx, pgoff 0x%lx, "
				"end 0x%lx, mm %p\n",
				vma, vma->vm_start, vma->vm_pgoff,
				vma->vm_end, vma->vm_mm);

			address = vma->vm_start +
				((pgoff - vma->vm_pgoff) << PAGE_SHIFT);

			if (address < vma->vm_start || address >= vma->vm_end) {
				bs2_info("%s: address not in vma area! "
					"vma start 0x%lx, end 0x%lx, "
					"pgoff 0x%lx, extent pgoff 0x%lx, "
					"address 0x%lx\n", __func__,
					vma->vm_start, vma->vm_end,
					vma->vm_pgoff, pgoff, address);
			}
#if 0
			count = extent->length >> bs2_dev->s_blocksize_bits;
			void_array = kzalloc(count, GFP_KERNEL);
			required = bankshot2_get_dirty_page_array(bs2_dev,
				NULL, extent, void_array, count);
			bs2_info("Extent 0x%lx, length %lu: %lu dirty pages\n",
				extent->offset, extent->length, required);
			kfree(void_array);
#endif
			vm_munmap_page(mm, vma->vm_start,
					vma->vm_end - vma->vm_start);

			list_del(&delete->list);
			kfree(delete);
		}
	}
}

int bankshot2_remove_mapping_from_tree(struct bankshot2_device *bs2_dev,
		struct bankshot2_inode *pi)
{
	struct mm_struct *mm = current->mm;
	struct extent_entry *curr;
	struct rb_node *temp;

//	write_lock(&pi->extent_tree_lock);
	temp = rb_first(&pi->extent_tree);
	while (temp) {
		curr = container_of(temp, struct extent_entry, node);
		bs2_dbg("pi %llu, extent offset 0x%lx, length %lu\n",
				pi->i_ino, curr->offset, curr->length);
		bankshot2_remove_mapping_from_extent(bs2_dev, curr, mm);
		temp = rb_next(temp);
	}

//	write_unlock(&pi->extent_tree_lock);
	return 0;
}

/* ========================= Physical Tree ============================ */

static inline int bankshot2_rbtree_compare_find_phy(struct extent_entry *curr,
		off_t b_offset)
{
	if ((curr->b_offset <= b_offset) &&
			(curr->b_offset + curr->length > b_offset))
		return 0;

	if (b_offset < curr->b_offset) return -1;
	if (b_offset > curr->b_offset) return 1;

	return 0;
}

struct extent_entry * bankshot2_find_physical_extent(
		struct bankshot2_device *bs2_dev, off_t b_offset)
{
	struct extent_entry *curr;
	struct rb_node *temp;
	int compVal;

	mutex_lock(&bs2_dev->phy_tree_lock);
	temp = bs2_dev->physical_tree.rb_node;
	while (temp) {
		curr = container_of(temp, struct extent_entry, node);
		compVal = bankshot2_rbtree_compare_find_phy(curr, b_offset);

		if (compVal == -1) {
			temp = temp->rb_left;
		} else if (compVal == 1) {
			temp = temp->rb_right;
		} else {
			mutex_unlock(&bs2_dev->phy_tree_lock);
			return curr;
		}
	}

	mutex_unlock(&bs2_dev->phy_tree_lock);
	return NULL;
}

int bankshot2_insert_physical_tree(struct bankshot2_device *bs2_dev,
		struct bankshot2_inode *pi, u64 extent_offset,
		size_t extent_length, u64 extent_b_offset)
{
	struct extent_entry *curr, *new, *prev, *next;
	struct rb_node **temp, *parent, *prev_node, *next_node;
	int compVal;
	int ret;

	temp = &(bs2_dev->physical_tree.rb_node);
	parent = NULL;

	mutex_lock(&bs2_dev->phy_tree_lock);
	while (*temp) {
		curr = container_of(*temp, struct extent_entry, node);
		compVal = bankshot2_rbtree_compare_find_phy(curr,
						extent_b_offset);
		parent = *temp;

		if (compVal == -1) {
			temp = &((*temp)->rb_left);
		} else if (compVal == 1) {
			temp = &((*temp)->rb_right);
		} else {
			if (curr->ino != pi->i_ino
					|| (extent_offset - curr->offset) !=
					(extent_b_offset - curr->b_offset)) {
				bs2_info("Existing physical extent hit but "
					"unmatch! existing extent ino %llu, "
					"offset 0x%lx, length %lu, "
					"b_offset 0x%lx, "
					"new extent ino %llu, offset 0x%llx, "
					"length %lu, b_offset 0x%llx\n",
					curr->ino,
					curr->offset, curr->length,
					curr->b_offset, pi->i_ino,
					extent_offset, extent_length,
					extent_b_offset);
				ret = 0;
				goto out;
			}
			if (extent_offset + extent_length <=
					curr->offset + curr->length) {
				ret = 0;
				goto out;
			} else {
				curr->length = extent_offset + extent_length
					- curr->offset;
				new = curr;
				goto check_next_overlap;
			}
		}
	}

	new = (struct extent_entry *)
		kmem_cache_alloc(bs2_dev->bs2_extent_slab, GFP_KERNEL);
	if (!new) {
//		write_unlock(&pi->extent_tree_lock);
		ret = -ENOMEM;
		goto out;
	}

	new->ino = pi->i_ino;
	new->offset = extent_offset;
	new->length = extent_length;
	new->b_offset = extent_b_offset;
	INIT_LIST_HEAD(&new->vma_list); // Not used

	rb_link_node(&new->node, parent, temp);
	rb_insert_color(&new->node, &bs2_dev->physical_tree);

	/* Check prev extent overlap */
	prev_node = rb_prev(&new->node);
	if (!prev_node)
		goto check_next_overlap;

	prev = container_of(prev_node, struct extent_entry, node);

	if ((prev->ino == new->ino) &&
	    (prev->offset + prev->length >= new->offset) &&
	    (prev->b_offset + (new->offset - prev->offset) == new->b_offset)) {
		if (prev->offset + prev->length < new->offset + new->length)
			prev->length = new->offset + new->length - prev->offset;

		rb_erase(&new->node, &bs2_dev->physical_tree);
		bankshot2_free_extent(bs2_dev, new);

		new = prev;
	}

check_next_overlap:
	while(1) {
		next_node = rb_next(&new->node);
		if (!next_node)
			break;
	
		next = container_of(next_node, struct extent_entry, node);

		if ((new->ino == next->ino) &&
		    (new->offset + new->length >= next->offset) &&
		    (new->b_offset + (next->offset - new->offset)
				== next->b_offset)) {
			if (next->offset + next->length > new->offset + new->length)
				new->length = next->offset + next->length - new->offset;

			rb_erase(&next->node, &bs2_dev->physical_tree);
		} else {
			break;
		}
	}

	ret = 0;
out:
	mutex_unlock(&bs2_dev->phy_tree_lock);
	return ret;
}

void bankshot2_destroy_physical_tree(struct bankshot2_device *bs2_dev)
{
	struct extent_entry *curr;
	struct rb_node *temp;

//	write_lock(&pi->extent_tree_lock);
	temp = rb_first(&bs2_dev->physical_tree);
	while (temp) {
		curr = container_of(temp, struct extent_entry, node);
//		bs2_info("pi %llu, extent offset %lu, length %lu, "
//				"mmap addr %lx\n", pi->i_ino, curr->offset,
//				curr->length, curr->mmap_addr);
		temp = rb_next(temp);
		rb_erase(&curr->node, &bs2_dev->physical_tree);
		bankshot2_free_extent(bs2_dev, curr);
	}

//	write_unlock(&pi->extent_tree_lock);
	bs2_info("%s returns.\n", __func__);
	return;
}

void bankshot2_print_physical_tree(struct bankshot2_device *bs2_dev)
{
	struct extent_entry *curr;
	struct rb_node *temp;

//	read_lock(&pi->extent_tree_lock);
	bs2_info("Print physical tree:\n");
	temp = rb_first(&bs2_dev->physical_tree);
	while (temp) {
		curr = container_of(temp, struct extent_entry, node);
		bs2_info("b_offset 0x%lx, pi %llu, extent offset 0x%lx, "
				"length %lu\n",	curr->b_offset, curr->ino,
				curr->offset, curr->length);
		temp = rb_next(temp);
	}

//	read_unlock(&pi->extent_tree_lock);
	return;
}

/* ========================== Access Tree ============================= */

static inline int bankshot2_rbtree_compare_overlap(struct extent_entry *curr,
		off_t offset, size_t count)
{
	if (offset + count <= curr->offset)
		return -1;

	if (curr->offset + curr->length <= offset)
		return 1;

	return 0;
}

int bankshot2_extent_being_accessed(struct bankshot2_device *bs2_dev,
		struct bankshot2_inode *pi, off_t pos, size_t count)
{
	struct extent_entry *curr;
	struct rb_node *temp;
	int compVal;

	temp = pi->access_tree.rb_node;
	while (temp) {
		curr = container_of(temp, struct extent_entry, node);
		compVal = bankshot2_rbtree_compare_overlap(curr, pos, count);

		if (compVal == -1) {
			temp = temp->rb_left;
		} else if (compVal == 1) {
			temp = temp->rb_right;
		} else {
			return 1;
		}
	}

	return 0;
}

int bankshot2_insert_access_extent(struct bankshot2_device *bs2_dev,
		struct bankshot2_inode *pi, off_t pos, size_t count)
{
	struct extent_entry *curr, *new;
	struct rb_node **temp, *parent;
	int compVal;

	temp = &(pi->access_tree.rb_node);
	parent = NULL;

	while (*temp) {
		curr = container_of(*temp, struct extent_entry, node);
		compVal = bankshot2_rbtree_compare_find(curr, pos);
		parent = *temp;

		if (compVal == -1) {
			temp = &((*temp)->rb_left);
		} else if (compVal == 1) {
			temp = &((*temp)->rb_right);
		} else {
			bs2_info("ERROR: Hit accessing extent! existing extent"
					" offset 0x%lx, length %lu, "
					"new extent offset 0x%lx, length %lu\n",
					curr->offset, curr->length,
					pos, count);
			return -EINVAL;
		}
	}

	new = (struct extent_entry *)
		kmem_cache_alloc(bs2_dev->bs2_extent_slab, GFP_KERNEL);
	if (!new)
		return -ENOMEM;

	new->offset = pos;
	new->length = count;
	INIT_LIST_HEAD(&new->vma_list); // Not used

	rb_link_node(&new->node, parent, temp);
	rb_insert_color(&new->node, &pi->access_tree);
	pi->num_access_extents++;

	return 0;
}

void bankshot2_remove_access_extent(struct bankshot2_device *bs2_dev,
			struct bankshot2_inode *pi, off_t pos, size_t count)
{
	struct extent_entry *curr;
	struct rb_node *temp;
	int compVal;

	temp = pi->access_tree.rb_node;
	while (temp) {
		curr = container_of(temp, struct extent_entry, node);
		compVal = bankshot2_rbtree_compare_find(curr, pos);

		if (compVal == -1) {
			temp = temp->rb_left;
		} else if (compVal == 1) {
			temp = temp->rb_right;
		} else {
			bs2_dbg("Delete extent to pi %llu, extent offset %lu, "
				"length %lu\n",
				pi->i_ino, curr->offset, curr->length);
			rb_erase(&curr->node, &pi->access_tree);
			pi->num_access_extents--;
			bankshot2_free_extent(bs2_dev, curr);
			break;
		}
	}

	return;
}

void bankshot2_print_access_tree(struct bankshot2_device *bs2_dev,
				struct bankshot2_inode *pi)
{
	struct extent_entry *curr;
	struct rb_node *temp;

	mutex_lock(&pi->tree_lock);

	if (pi->num_access_extents)
		bs2_info("Print access tree for pi %llu, %u extents\n",
				pi->i_ino, pi->num_access_extents);
	temp = rb_first(&pi->access_tree);
	while (temp) {
		curr = container_of(temp, struct extent_entry, node);
		bs2_info("pi %llu, access extent offset %lu, length %lu\n",
				pi->i_ino, curr->offset, curr->length);
		temp = rb_next(temp);
	}

	mutex_unlock(&pi->tree_lock);
	return;
}

void bankshot2_delete_access_tree(struct bankshot2_device *bs2_dev,
				struct bankshot2_inode *pi)
{
	struct extent_entry *curr;
	struct rb_node *temp;

	temp = rb_first(&pi->access_tree);
	while (temp) {
		curr = container_of(temp, struct extent_entry, node);
		bs2_info("pi %llu, access extent offset %lu, length %lu\n",
				pi->i_ino, curr->offset, curr->length);
		temp = rb_next(temp);
		rb_erase(&curr->node, &pi->access_tree);
		bankshot2_free_extent(bs2_dev, curr);
	}

	pi->num_access_extents = 0;
	return;
}

/* ============================= Init code =============================== */

int bankshot2_init_extents(struct bankshot2_device *bs2_dev)
{
	bs2_dev->bs2_extent_slab = kmem_cache_create(
					"bankshot2_extent_slab",
					sizeof(struct extent_entry),
					0, 0, NULL);
	if (bs2_dev->bs2_extent_slab == NULL)
		return -ENOMEM;
	return 0;
}

void bankshot2_destroy_extents(struct bankshot2_device *bs2_dev)
{
	int i;
	struct bankshot2_inode *pi;

	for (i = BANKSHOT2_FREE_INODE_HINT_START;
			i < bs2_dev->s_inodes_count; i++) {
		pi = bankshot2_get_inode(bs2_dev, i);
		if (pi && pi->num_extents) {
			bs2_dbg("pi %llu: %u extents\n",
					pi->i_ino, pi->num_extents);
			bankshot2_delete_tree(bs2_dev, pi);
		}
		if (pi && pi->num_access_extents) {
			bs2_info("pi %llu: still have %u access extents\n",
					pi->i_ino, pi->num_access_extents);
			bankshot2_delete_access_tree(bs2_dev, pi);
		}
	}

	kmem_cache_destroy(bs2_dev->bs2_extent_slab);
	bs2_info("%s returns.\n", __func__);
}

