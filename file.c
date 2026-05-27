// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>

#include "ouichefs.h"
#include "bitmap.h"

uint32_t reservation_size = 8;
module_param(reservation_size, uint, 0644);
MODULE_PARM_DESC(reservation_size, "Taille de la fenetre de reservation en blocs");

/*
 * Map the buffer_head passed in argument with the iblock-th block of the file
 * represented by inode. If the requested block is not allocated and create is
 * true, allocate a new block on disk and map it.
 */


static int ouichefs_file_get_block(struct inode *inode, sector_t iblock,
				   struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct ouichefs_file_index_block *index;
	struct buffer_head *bh_index;
	int ret = 0, bno;

	/* If block number exceeds filesize, fail */
	if (iblock >= OUICHEFS_BLOCK_SIZE >> 2)
		return -EFBIG;

	/* Read index block from disk */
	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index)
		return -EIO;
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	/*
	 * Check if iblock is already allocated. If not and create is true,
	 * allocate it. Else, get the physical block number.
	 */
	if (index->extents[iblock].count == 0) {//en rapport avec ce que nous dit le sujet là
		if (!create) {
			ret = 0;
			goto brelse_index;
		}
		bno = get_free_block(sbi);
		if (!bno) {
			ret = -ENOSPC;
			goto brelse_index;
		}
		index->extents[iblock].start = cpu_to_le32(bno);
		mark_buffer_dirty(bh_index);
	} else {
		bno = le32_to_cpu(index->extents[iblock].start);
	}

	/* Map the physical block to the given buffer_head */
	map_bh(bh_result, sb, bno);

brelse_index:
	brelse(bh_index);

	return ret;
}

/*
 * Called by the page cache to read a page from the physical disk and map it in
 * memory.
 */
static void ouichefs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, ouichefs_file_get_block);
}

/*
 * Called by the page cache to write a dirty page to the physical disk (when
 * sync is called or when memory is needed).
 */
static int ouichefs_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, ouichefs_file_get_block, wbc);
}

/*
 * Called by the VFS when a write() syscall occurs on file before writing the
 * data in the page cache. This functions checks if the write will be able to
 * complete and allocates the necessary blocks through block_write_begin().
 */
static int ouichefs_write_begin(struct file *file,
				struct address_space *mapping, loff_t pos,
				unsigned int len, struct page **pagep,
				void **fsdata)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(file->f_inode->i_sb);
	int err;
	uint32_t nr_allocs = 0;

	/* Check if the write can be completed (enough space?) */
	if (pos + len > OUICHEFS_MAX_FILESIZE)
		return -ENOSPC;
	nr_allocs = max(pos + len, file->f_inode->i_size) / OUICHEFS_BLOCK_SIZE;
	if (nr_allocs > file->f_inode->i_blocks - 1)
		nr_allocs -= file->f_inode->i_blocks - 1;
	else
		nr_allocs = 0;
	if (nr_allocs > sbi->nr_free_blocks)
		return -ENOSPC;

	/* prepare the write */
	err = block_write_begin(mapping, pos, len, pagep,
				ouichefs_file_get_block);
	/* if this failed, reclaim newly allocated blocks */
	if (err < 0) {
		pr_err("%s:%d: newly allocated blocks reclaim not implemented yet\n",
		       __func__, __LINE__);
	}
	return err;
}

/*
 * Called by the VFS after writing data from a write() syscall to the page
 * cache. This functions updates inode metadata and truncates the file if
 * necessary.
 */
static int ouichefs_write_end(struct file *file, struct address_space *mapping,
			      loff_t pos, unsigned int len, unsigned int copied,
			      struct page *page, void *fsdata)
{
	int ret;
	struct inode *inode = file->f_inode;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;

	/* Complete the write() */
	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (ret < len) {
		pr_err("%s:%d: wrote less than asked... what do I do? nothing for now...\n",
		       __func__, __LINE__);
	} else {
		uint32_t nr_blocks_old = inode->i_blocks;

		/* Update inode metadata */
		inode->i_blocks = (roundup(inode->i_size, OUICHEFS_BLOCK_SIZE) /
				   OUICHEFS_BLOCK_SIZE) +
				  1;
		inode->i_mtime = inode->i_ctime = current_time(inode);
		mark_inode_dirty(inode);

		/* If file is smaller than before, free unused blocks */
		if (nr_blocks_old > inode->i_blocks) {
			int i;
			struct buffer_head *bh_index;
			struct ouichefs_file_index_block *index;

			/* Free unused blocks from page cache */
			truncate_pagecache(inode, inode->i_size);

			/* Read index block to remove unused blocks */
			bh_index = sb_bread(sb, ci->index_block);
			if (!bh_index) {
				pr_err("failed truncating '%s'. we just lost %llu blocks\n",
				       file->f_path.dentry->d_name.name,
				       nr_blocks_old - inode->i_blocks);
				goto end;
			}
			index = (struct ouichefs_file_index_block *)
					bh_index->b_data;

			for (i = inode->i_blocks - 1; i < nr_blocks_old - 1;
			     i++) {
				put_block(OUICHEFS_SB(sb), le32_to_cpu(index->extents[i].start));
				index->extents[i].start = 0;//ici on gere le cas ou le fichier rétrecit
				index->extents[i].count = 0;
			}
			mark_buffer_dirty(bh_index);
			brelse(bh_index);
		}
	}
end:
	return ret;
}

const struct address_space_operations ouichefs_aops = {
	.readahead = ouichefs_readahead,
	.writepage = ouichefs_writepage,
	.write_begin = ouichefs_write_begin,
	.write_end = ouichefs_write_end
};

static int ouichefs_open(struct inode *inode, struct file *file)
{
	bool wronly = (file->f_flags & O_WRONLY) != 0;
	bool rdwr = (file->f_flags & O_RDWR) != 0;
	bool trunc = (file->f_flags & O_TRUNC) != 0;

	if ((wronly || rdwr) && trunc && (inode->i_size != 0)) {
		struct super_block *sb = inode->i_sb;
		struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
		struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
		struct ouichefs_file_index_block *index;
		struct buffer_head *bh_index;
		sector_t iblock;

        //Libération des reservation
        if (ci->i_reserved_count > 0) {
			uint32_t k;
			for (k = 0; k < ci->i_reserved_count; k++)
				put_block(sbi, ci->i_reserved_start + k);
			ci->i_reserved_start = 0;
			ci->i_reserved_count = 0;
		}

		/* Read index block from disk */
		bh_index = sb_bread(sb, ci->index_block);
		if (!bh_index)
			return -EIO;
		index = (struct ouichefs_file_index_block *)bh_index->b_data;

		//Boucle de parcours des extents
		for (iblock = 0; iblock < OUICHEFS_MAX_EXTENTS; iblock++) {
			//Récuperation de start et count + conversation
			uint32_t start = le32_to_cpu(index->extents[iblock].start);
			uint32_t count = le32_to_cpu(index->extents[iblock].count);

			//Si count =0 alors tout ce qui suit vaut 0 aussi 
			if(count == 0){
				break;
			}

			//Parcours de chaque bloc de l'extent courant
			for(uint32_t j = 0; j < count; j++){
				put_block(sbi, start + j);   //start = num ddu premier bloc physique du extent, j = indice du bloc dans le extent
			}
			index->extents[iblock].start = 0;
			index->extents[iblock].count = 0;
		}
		inode->i_size = 0;
		inode->i_blocks = 1;

		mark_buffer_dirty(bh_index);
		brelse(bh_index);
	}

	return 0;
}

static int ouichefs_release(struct inode *inode, struct file *file)
{
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	uint32_t k;

	// Libérer les blocs réservés non utilisés à la fermeture
	// ces blocs sont alloués dans le bitmap 
	if (ci->i_reserved_count > 0) {
		for (k = 0; k < ci->i_reserved_count; k++)
			put_block(OUICHEFS_SB(sb),ci->i_reserved_start + k);
		ci->i_reserved_start = 0;
		ci->i_reserved_count = 0;
	}

	return 0;
}

static uint32_t ouichefs_alloc_contiguous(struct super_block *sb,
                                           uint32_t requested,
                                           uint32_t *block)
{
    struct ouichefs_sb_info *sbi      = OUICHEFS_SB(sb);
    unsigned long           *bitmap   = sbi->bfree_bitmap;
    unsigned long            nr_blocks = sbi->nr_blocks;

    uint32_t best_start = 0;
    uint32_t best_len   = 0;
    uint32_t cur        = 0;

    while (cur < nr_blocks) {
        uint32_t run_start = find_next_bit(bitmap, nr_blocks, cur);
        if (run_start >= nr_blocks)
            break;

        uint32_t run_end = find_next_zero_bit(bitmap, nr_blocks, run_start);
        uint32_t run_len = min((uint32_t)(run_end - run_start), requested);

        if (run_len > best_len) {
            best_start = run_start;
            best_len   = run_len;
        }

        if (best_len >= requested)
            break;

        cur = run_end;
    }

    if (best_len == 0)
        return 0;

    bitmap_clear(bitmap, best_start, best_len);
    sbi->nr_free_blocks -= best_len;

    *block = best_start;
    return best_len;
}

static int ouichefs_last_extent(struct ouichefs_file_index_block *index)
{
    int i = 0;
    while (i < OUICHEFS_MAX_EXTENTS && index->extents[i].count != 0)
        i++;
    return i;
}

// static int ouichefs_alloc_and_register(struct super_block *sb,
//                                         struct ouichefs_file_index_block *index,
//                                         uint32_t remaining_blocks,
//                                         uint32_t *phys_out,
//                                         uint32_t *allocated_out)
// {
//     struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
//     uint32_t bno;

//     uint32_t allocated = ouichefs_alloc_contiguous(sb, remaining_blocks, &bno);
//     if (!allocated)
//         return -ENOSPC;

//     int i = ouichefs_last_extent(index);

//     if (i > 0) {
//         struct ouichefs_extent *last = &index->extents[i - 1];
//         uint32_t s = le32_to_cpu(last->start);
//         uint32_t c = le32_to_cpu(last->count);

//         if (s + c == bno) {
//             /* Contigu → étendre le dernier extent */
//             last->count = cpu_to_le32(c + allocated);
//         } else {
//             /* Non contigu → nouvel extent */
//             if (i >= OUICHEFS_MAX_EXTENTS) {
//                 for (uint32_t k = 0; k < allocated; k++)
//                     put_block(sbi, bno + k);
//                 return -ENOSPC;
//             }
//             index->extents[i].start = cpu_to_le32(bno);
//             index->extents[i].count = cpu_to_le32(allocated);
//         }
//     } else {
//         /* Premier extent du fichier */
//         index->extents[0].start = cpu_to_le32(bno);
//         index->extents[0].count = cpu_to_le32(allocated);
//     }

//     *phys_out      = bno;
//     *allocated_out = allocated;
//     return 0;
// }


static int ouichefs_write_chunk(struct super_block *sb,
                                 uint32_t phys_block,
                                 const char __user *buf,
                                 uint32_t offset_in_block,
                                 uint32_t len)
{
    struct buffer_head *bh = sb_bread(sb, phys_block);
    if (!bh)
        return -EIO;

    if (copy_from_user(bh->b_data + offset_in_block, buf, len)) {
        brelse(bh);
        return -EFAULT;
    }

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    return 0;
}

static uint32_t ouichefs_extent_get_block(struct ouichefs_extent *extents, uint32_t logical_block){
	
	uint32_t i=0;
	int ret=0;
	while(extents[i].count != 0 && i<OUICHEFS_MAX_EXTENTS){
		uint32_t start = le32_to_cpu(extents[i].start);
		uint32_t count = le32_to_cpu(extents[i].count);
		if(logical_block >= count) {
			logical_block -= count;
			i++;
		}
		else{
			if (start == 0)
                ret = OUICHEFS_HOLE_BLOCK;  
            else
                ret = start + logical_block; 
            break;
		}
	}
	return ret;
}

/* Retourne l'index de l'extent contenant logical_block,
 * et l'offset de logical_block dans cet extent */
static int ouichefs_find_extent(struct ouichefs_file_index_block *index,
                                 uint32_t logical_block,
                                 uint32_t *offset_in_extent)
{
    int i = 0;
    uint32_t remaining = logical_block;

    while (i < OUICHEFS_MAX_EXTENTS &&
           index->extents[i].count != 0) {
        uint32_t count = le32_to_cpu(index->extents[i].count);

        if (remaining < count) {
            *offset_in_extent = remaining;
            return i;
        }
        remaining -= count;
        i++;
    }
    return -1;
}

/* Décale tous les extents à partir de pos d'un cran vers la droite
 * pour libérer un slot à l'index pos */
static int ouichefs_shift_extents_right(
        struct ouichefs_file_index_block *index, int pos)
{
    int last = ouichefs_last_extent(index);

    /* Vérifier qu'il reste de la place */
    if (last >= OUICHEFS_MAX_EXTENTS)
        return -ENOSPC;

    /* Copier de droite à gauche pour éviter d'écraser
     * les données source avant de les avoir copiées */
    for (int j = last; j >= pos; j--)
        index->extents[j + 1] = index->extents[j];

    return 0;
}

/* Découpe un trou existant et alloue un vrai bloc pour la zone écrite */
static int ouichefs_write_into_hole(struct super_block *sb,
                                     struct ouichefs_file_index_block *index,
                                     uint32_t logical_block,
                                     uint32_t *phys_out)
{
    struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
    uint32_t offset_in_extent;
    int ei, ret;
    uint32_t new_bno;

    /* 1. Trouver l'extent trou qui contient ce bloc logique */
    ei = ouichefs_find_extent(index, logical_block, &offset_in_extent);
    if (ei < 0)
        return -EIO;

    uint32_t hole_count = le32_to_cpu(index->extents[ei].count);

    /* 2. Allouer un vrai bloc physique */
    new_bno = get_free_block(sbi);
    if (!new_bno)
        return -ENOSPC;

    /* 3. Découper selon la position dans le trou */

    if (hole_count == 1) {
        /* Cas simple : trou d'un seul bloc
         * {0, 1} → {réel, 1} */
        index->extents[ei].start = cpu_to_le32(new_bno);
        index->extents[ei].count = cpu_to_le32(1);

    } else if (offset_in_extent == 0) {
        /* Écriture au DÉBUT du trou
         * {0, N} → {réel, 1}, {0, N-1} */
        ret = ouichefs_shift_extents_right(index, ei);
        if (ret) {
            put_block(sbi, new_bno);
            return ret;
        }
        /* Nouveau bloc réel */
        index->extents[ei].start   = cpu_to_le32(new_bno);
        index->extents[ei].count   = cpu_to_le32(1);
        /* Reste du trou */
        index->extents[ei+1].start = cpu_to_le32(0);
        index->extents[ei+1].count = cpu_to_le32(hole_count - 1);

    } else if (offset_in_extent == hole_count - 1) {
        /* Écriture à la FIN du trou
         * {0, N} → {0, N-1}, {réel, 1} */
        ret = ouichefs_shift_extents_right(index, ei + 1);
        if (ret) {
            put_block(sbi, new_bno);
            return ret;
        }
        /* Partie gauche du trou */
        index->extents[ei].count     = cpu_to_le32(hole_count - 1);
        /* Nouveau bloc réel */
        index->extents[ei+1].start   = cpu_to_le32(new_bno);
        index->extents[ei+1].count   = cpu_to_le32(1);

    } else {
        /* Écriture au MILIEU du trou
         * {0, N} → {0, offset}, {réel, 1}, {0, N-offset-1}
         * Nécessite 2 slots supplémentaires */
        int last = ouichefs_last_extent(index);
        if (last + 2 > OUICHEFS_MAX_EXTENTS) {
            put_block(sbi, new_bno);
            return -ENOSPC;
        }

        /* Décaler de 2 crans vers la droite à partir de ei
         * pour faire place aux 2 nouveaux extents */
        for (int j = last; j >= ei; j--)
            index->extents[j + 2] = index->extents[j];

        /* Partie gauche du trou */
        index->extents[ei].start   = cpu_to_le32(0);
        index->extents[ei].count   = cpu_to_le32(offset_in_extent);
        /* Bloc réel au milieu */
        index->extents[ei+1].start = cpu_to_le32(new_bno);
        index->extents[ei+1].count = cpu_to_le32(1);
        /* Partie droite du trou */
        index->extents[ei+2].start = cpu_to_le32(0);
        index->extents[ei+2].count = cpu_to_le32(
            hole_count - offset_in_extent - 1);
    }

    *phys_out = new_bno;
    return 0;
}




static ssize_t ouichefs_read(struct file *file, char __user *buf,
                              size_t count, loff_t *pos)
{
    struct inode *inode = file->f_inode;
    struct super_block *sb = inode->i_sb;
    struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
    struct buffer_head *bh_index = NULL;
    struct buffer_head *bh_data = NULL;
    struct ouichefs_file_index_block *index;
    size_t len = count;
    int ret = 0;
    loff_t new_pos = *pos;
    int total_read = 0;

    /* EOF check */
    if (new_pos >= inode->i_size) {
        ret = 0;
        goto end;
    }

    /* Ajuster len pour ne pas lire au-delà de i_size */
    if (new_pos + len > inode->i_size)
        len = inode->i_size - new_pos;

    /* Lire le bloc index */
    bh_index = sb_bread(sb, ci->index_block);
    if (!bh_index) {
        ret = -EIO;
        goto end;
    }
    index = (struct ouichefs_file_index_block *)bh_index->b_data;

    while (len > 0) {
        uint32_t logical_block    = new_pos / OUICHEFS_BLOCK_SIZE;
        uint32_t offset_in_block  = new_pos % OUICHEFS_BLOCK_SIZE;
        uint32_t available_in_block = OUICHEFS_BLOCK_SIZE - offset_in_block;
        if (available_in_block > len)
            available_in_block = len;

        /* Traduction logique → physique via le helper 1.4.1 */
        uint32_t phys = ouichefs_extent_get_block(index->extents, logical_block);

		if (phys == 0) {
			/* Fin de fichier — arrêter */
			break;

		} else if (phys == OUICHEFS_HOLE_BLOCK) {
			/* Trou — allouer un buffer kernel, le mettre à zéro,
			* puis le copier vers userspace */
			char zero_buf[OUICHEFS_BLOCK_SIZE];
			memset(zero_buf, 0, available_in_block);

			unsigned long not_copied = copy_to_user(buf + total_read,
													zero_buf,
													available_in_block);
			size_t copied = available_in_block - not_copied;
			total_read += copied;
			new_pos    += copied;
			len        -= copied;

		} else {
			/* Bloc physique réel — lire normalement */
			bh_data = sb_bread(sb, phys);
			if (!bh_data) {
				ret = -EIO;
				goto brelse_index;
			}
			unsigned long not_copied = copy_to_user(buf + total_read,
													bh_data->b_data + offset_in_block,
													available_in_block);
			brelse(bh_data);

			size_t copied = available_in_block - not_copied;
			total_read += copied;
			new_pos    += copied;
			len        -= copied;
		}
    }

    *pos = new_pos;
    ret = total_read;

brelse_index:
    brelse(bh_index);

end:
    return ret;
}


static void ouichefs_gc(struct super_block *sb)
{
	struct inode *inode;

	spin_lock(&sb->s_inode_list_lock);

	//Compter le nombre d'appel à gc
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	sbi->gc_runs++;

    //Parcour des inodes chargé en mémoire
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
		uint32_t k;

		//libération  de l'ensemble de ces blocs reservé
		for (k = 0; k < ci->i_reserved_count; k++)
			put_block(OUICHEFS_SB(sb),ci->i_reserved_start + k);
		ci->i_reserved_start = 0;
		ci->i_reserved_count = 0;
		
	}
	spin_unlock(&sb->s_inode_list_lock);
}

static ssize_t ouichefs_write(struct file *file, const char __user *buf,
                               size_t count, loff_t *pos)
{
    struct inode               *inode = file->f_inode;
    struct super_block         *sb    = inode->i_sb;
    struct ouichefs_sb_info    *sbi   = OUICHEFS_SB(sb);
    struct ouichefs_inode_info *ci    = OUICHEFS_INODE(inode);
    struct buffer_head         *bh_index = NULL;
    struct ouichefs_file_index_block *index;
    size_t len           = count;
    int    ret           = 0;
    loff_t new_pos       = *pos;
    int    total_written = 0;

    inode_lock(inode);

    if (file->f_flags & O_APPEND)
        new_pos = inode->i_size;

    /* Vérification de place */
    uint32_t nr_allocs =
        max((loff_t)(new_pos + len), inode->i_size) / OUICHEFS_BLOCK_SIZE;
    if (nr_allocs > inode->i_blocks - 1)
        nr_allocs -= inode->i_blocks - 1;
    else
        nr_allocs = 0;
    if (nr_allocs > sbi->nr_free_blocks) {
        ret = -ENOSPC;
        goto out_unlock;
    }

    bh_index = sb_bread(sb, ci->index_block);
    if (!bh_index) { ret = -EIO; goto out_unlock; }
    index = (struct ouichefs_file_index_block *)bh_index->b_data;


	if (new_pos > inode->i_size) {
		/* Calculer le gap en blocs */
		uint32_t gap_bytes  = new_pos - inode->i_size;
		uint32_t gap_blocks = (gap_bytes + OUICHEFS_BLOCK_SIZE - 1)
							/ OUICHEFS_BLOCK_SIZE;

		int ei = ouichefs_last_extent(index);

		/* Si le dernier extent est déjà un trou, l'étendre */
		if (ei > 0 && index->extents[ei-1].start == 0) {
			uint32_t c = le32_to_cpu(index->extents[ei-1].count);
			index->extents[ei-1].count = cpu_to_le32(c + gap_blocks);
		} else {
			/* Sinon créer un nouveau trou */
			if (ei >= OUICHEFS_MAX_EXTENTS)
				return -ENOSPC;
			index->extents[ei].start = cpu_to_le32(0);
			index->extents[ei].count = cpu_to_le32(gap_blocks);
		}
		mark_buffer_dirty(bh_index);
	}



    while (len > 0) {
        uint32_t logical_block   = new_pos / OUICHEFS_BLOCK_SIZE;
        uint32_t offset_in_block = new_pos % OUICHEFS_BLOCK_SIZE;
        uint32_t available       = min((uint32_t)(OUICHEFS_BLOCK_SIZE - offset_in_block),
                                       (uint32_t)len);

        uint32_t phys_block =
            ouichefs_extent_get_block(index->extents, logical_block);

		if (phys_block == OUICHEFS_HOLE_BLOCK) {
		/* Écriture dans un trou existant → découper le trou */
		ret = ouichefs_write_into_hole(sb, index, logical_block,
										&phys_block);
		if (ret)
			goto brelse_index;
		mark_buffer_dirty(bh_index);

        } else if (phys_block == 0) {
            uint32_t new_bno;

            //Reserve disponible
            if (ci->i_reserved_count > 0) {     

                new_bno = ci->i_reserved_start; //Le prochain bloc à écrire
                ci->i_reserved_start++;
                ci->i_reserved_count--;
            } else {
                //Plus de reserve
                uint32_t start;
                uint32_t got;
                uint32_t to_request = max(reservation_size,
                    (uint32_t)(roundup(new_pos + len,
                        OUICHEFS_BLOCK_SIZE) / OUICHEFS_BLOCK_SIZE
                        - logical_block));

                got = ouichefs_alloc_contiguous(sb, to_request, &start);//Nouvelle fenetre de bloc
                //s'il n'y a plus de bloc libre
                if (!got) {
                    ouichefs_gc(sb);
                    got = ouichefs_alloc_contiguous(sb,to_request, &start);
                    //Si il n'y a toujours pas de bloc libre
                    if (!got) {
                        ret = -ENOSPC;
                        goto brelse_index;
                    }
                }

                //On stock la fenètre obtenu et on consomme le premier
                ci->i_reserved_start = start + 1;
                ci->i_reserved_count = got - 1; 
                new_bno = start;
            }


            int ei = ouichefs_last_extent(index);//Numero du premier slot vide dans le tableau extent

           
            if (ei > 0) {
                struct ouichefs_extent *last = &index->extents[ei - 1]; //pointeur vers le dernier extent valide
                uint32_t s = le32_to_cpu(last->start);
                uint32_t c = le32_to_cpu(last->count);

                //Contigus
                if (s + c == new_bno) {
                    
                    last->count = cpu_to_le32(c + 1);
                } 
                //Non contigu
                else {
                    //Si on a atteint le max extent
                    if (ei >= OUICHEFS_MAX_EXTENTS) {
                        ret = -ENOSPC;
                        goto brelse_index;
                    }
                    //Creer un nouvelle extent
                    index->extents[ei].start = cpu_to_le32(new_bno);
                    index->extents[ei].count = cpu_to_le32(1);
                }
            } 
             //Si le fichier n'a encore aucun extent
            else {
                index->extents[0].start = cpu_to_le32(new_bno);
                index->extents[0].count = cpu_to_le32(1);
            }

            mark_buffer_dirty(bh_index);
            phys_block = new_bno;
        }

        ret = ouichefs_write_chunk(sb, phys_block,
                                    buf + total_written,
                                    offset_in_block, available);
        if (ret) goto brelse_index;

        new_pos       += available;
        total_written += available;
        len           -= available;

        if (new_pos > inode->i_size)
            inode->i_size = new_pos;
    }

    if (total_written > 0) {
        inode->i_blocks = (roundup(inode->i_size, OUICHEFS_BLOCK_SIZE)
                           / OUICHEFS_BLOCK_SIZE) + 1;
        inode->i_mtime = inode->i_ctime = current_time(inode);
        mark_inode_dirty(inode);
    }
    *pos = new_pos;
    ret  = total_written;

brelse_index:
    sync_dirty_buffer(bh_index);
    brelse(bh_index);
out_unlock:
    inode_unlock(inode);
    return ret;
}




static long ouichefs_ioctl(struct file *file, unsigned int cmd, unsigned long arg){
	struct inode *inode = file->f_inode;
	struct super_block *sb = inode -> i_sb;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct ouichefs_file_index_block *index;
	struct buffer_head *bh_index;

	int i, nb_extents =0;

	switch (cmd){
		case OUICHEFS_IOC_GET_EXTENTS:
			//Lectuure de l'index block
			bh_index = sb_bread(sb, ci->index_block);
			if(!bh_index){
				return -EIO;
			}
			index = (struct ouichefs_file_index_block *)bh_index->b_data;

			//Nombre de extent.count pas égale à 0, on break dès qu'un seul vaut 0, la suite sera forcément 0
			for(i = 0; i < OUICHEFS_MAX_EXTENTS; i++){
				if(index->extents[i].count == 0){
					break;
				}
				nb_extents++;
			}
			
			//Afichage du header
			pr_info("extents for inode %lu: %d extent(s)\n", inode->i_ino, nb_extents);

			//Affichage des extents
			for (i = 0; i < nb_extents; i++) {
				uint32_t start = index->extents[i].start;
            	uint32_t count = index->extents[i].count;
            	pr_info("[%d] start=%u count=%u (blocks %u-%u)\n", i, start, count, start, start + count - 1);
        	}

			brelse(bh_index);
			return 0;
		default:
        	return -ENOTTY;  //Commande inconnu
	}

}



const struct file_operations ouichefs_file_ops = {
	.owner = THIS_MODULE,
	.open = ouichefs_open,
    .release = ouichefs_release,
	.llseek = generic_file_llseek,
	.read = ouichefs_read,
	.write = ouichefs_write,
	.fsync = generic_file_fsync,
	.unlocked_ioctl = ouichefs_ioctl,
};

