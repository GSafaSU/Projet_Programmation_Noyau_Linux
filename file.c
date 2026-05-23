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
	if (index->blocks[iblock].count == 0) {//en rapport avec ce que nous dit le sujet là
		if (!create) {
			ret = 0;
			goto brelse_index;
		}
		bno = get_free_block(sbi);
		if (!bno) {
			ret = -ENOSPC;
			goto brelse_index;
		}
		index->blocks[iblock].start = cpu_to_le32(bno);
		mark_buffer_dirty(bh_index);
	} else {
		bno = le32_to_cpu(index->blocks[iblock].start);
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
				put_block(OUICHEFS_SB(sb), le32_to_cpu(index->blocks[i].start));
				index->blocks[i].start = 0;//ici on gere le cas ou le fichier rétrecit
				index->blocks[i].count = 0;
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

		/* Read index block from disk */
		bh_index = sb_bread(sb, ci->index_block);
		if (!bh_index)
			return -EIO;
		index = (struct ouichefs_file_index_block *)bh_index->b_data;

		for (iblock = 0; index->blocks[iblock].count != 0; iblock++) {
			put_block(sbi, le32_to_cpu(index->blocks[iblock].start));
			index->blocks[iblock].start = 0;
			index->blocks[iblock].count = 0;
		}
		inode->i_size = 0;
		inode->i_blocks = 1;

		mark_buffer_dirty(bh_index);
		brelse(bh_index);
	}

	return 0;
}

static ssize_t ouichefs_read(struct file *file, char __user *buf,
                              size_t count, loff_t *pos)
{
    /* 1. Récupérer inode, sb, ci */
    struct inode *inode = file->f_inode;
    struct super_block *sb = inode->i_sb;
    struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);

    /* 2. EOF check : rien à lire si on est déjà à la fin */
    if (*pos >= inode->i_size)
        return 0;

    /* 3. Ajuster count pour ne pas lire au-delà de i_size */
    if (*pos + count > inode->i_size)
        count = inode->i_size - *pos;
        /* sans ça on lirait des octets hors fichier (garbage) */

    /* 4. Lire le bloc index */
    struct buffer_head *bh_index = sb_bread(sb, ci->index_block);
    if (!bh_index)
        return -EIO;
    struct ouichefs_file_index_block *index =
        (struct ouichefs_file_index_block *)bh_index->b_data;

    /* 5. Calculer le bloc logique et l'offset dans ce bloc */
    uint32_t logical_block = *pos / OUICHEFS_BLOCK_SIZE;
    uint32_t offset_in_block = *pos % OUICHEFS_BLOCK_SIZE;
    /* offset_in_block : où dans le bloc on commence à lire */

    /* 6. Récupérer le numéro de bloc physique */
    uint32_t phys_block = le32_to_cpu(index->blocks[logical_block].start);
    if (!phys_block) {
        /* bloc non alloué = trou dans le fichier */
        brelse(bh_index);
        return -EIO;
    }

    /* 7. Lire le bloc de données */
    struct buffer_head *bh_data = sb_bread(sb, phys_block);
    if (!bh_data) {
        brelse(bh_index);
        return -EIO;
    }

    /* 8. Limiter count à ce qui reste dans ce bloc */
    uint32_t available_in_block = OUICHEFS_BLOCK_SIZE - offset_in_block;
    if (count > available_in_block)
        count = available_in_block;
        /* on ne lit qu'un seul bloc à la fois */

    /* 9. Copier vers userspace depuis b_data + offset */
    size_t to_copy = min(count, (size_t)(OUICHEFS_BLOCK_SIZE - offset_in_block)); //Pour ne pas lire au delà de la fin de ce bloc actuel

	unsigned long not_copied = copy_to_user(buf,bh_data->b_data + offset_in_block, to_copy);
	ssize_t total_read = to_copy - not_copied;
    /* total_read = ce qui a été réellement copié */

    /* 10. Avancer le curseur */
    *pos += total_read;

    /* 11. Libérer les buffer_heads */
    brelse(bh_data);
    brelse(bh_index);

    return total_read;
}


static ssize_t ouichefs_write(struct file *file, const char __user *buf,
                              size_t count, loff_t *pos)
{
    struct inode *inode = file->f_inode;
    struct super_block *sb = inode->i_sb;
    struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
    struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
    
    struct buffer_head *bh_index = NULL;
    struct buffer_head *bh_data = NULL;
    struct ouichefs_file_index_block *index;

    size_t len = count;
    int ret = 0;
    loff_t new_pos = *pos;
    uint32_t nr_allocs = 0;
    int total_written = 0;

    inode_lock(inode);

    if(file->f_flags & O_APPEND)
        new_pos = file->f_inode->i_size; 
    
    /* Check if the write can be completed (enough space?) */
    if (new_pos + len > OUICHEFS_MAX_FILESIZE) {
        len = inode->i_size - new_pos;
    }

    nr_allocs = max((loff_t)(new_pos + len), inode->i_size) / OUICHEFS_BLOCK_SIZE;

    if (nr_allocs > inode->i_blocks - 1)
        nr_allocs -= inode->i_blocks - 1;
    else
        nr_allocs = 0;

	/*si il n'y a plus assez de block libre dans le buffer super_block*/
    if (nr_allocs > sbi->nr_free_blocks) {
        ret = -ENOSPC;
        goto out_unlock;
    }

    bh_index = sb_bread(sb, ci->index_block);
    if (!bh_index) {
        ret = -EIO;
        goto out_unlock;
    }
    /*recuperation de l'index*/
    index = (struct ouichefs_file_index_block *)bh_index->b_data;
    
    while (len > 0) {
        int new_block = 0; 

        uint32_t logical_block = new_pos / OUICHEFS_BLOCK_SIZE;
        uint32_t offset_in_block = new_pos % OUICHEFS_BLOCK_SIZE;

        uint32_t available_in_block = OUICHEFS_BLOCK_SIZE - offset_in_block;
        if (available_in_block > len)
            available_in_block = len;

        uint32_t phys_block = ouichefs_extent_get_block(index->blocks,logical_block);

        if (!phys_block) {
            /* bloc non alloué = trou dans le fichier */
			
            new_block = 1;
            phys_block = get_free_block(sbi);
			 if (!phys_block) {
                ret = -ENOSPC;
                goto brelse_index;
            }
			int i=0;
			//recuperation du dernière extends
			while(index->blocks[i].count != 0 && i<OUICHEFS_MAX_EXTENTS){
				i++;
			}
			
			if(i>0){
				struct ouichefs_extent *last = &index->blocks[i-1];
				uint32_t start = le32_to_cpu(last->start);
        		uint32_t cnt   = le32_to_cpu(last->count);
				if((start +cnt)== phys_block){//contigue etendre le derniere extent
					last->count=cpu_to_le32(cnt + 1);
				}else{//non contigue -> new extent
					uint32_t new_ext_index = i;
					if(new_ext_index >= OUICHEFS_MAX_EXTENTS){
						ret= -ENOSPC;
						goto brelse_index;
					}
					index->blocks[new_ext_index].start= cpu_to_le32(phys_block);
					index->blocks[new_ext_index].count= cpu_to_le32(1);
					
				}
			}else{//cas ou c'est le 1er extent du fichier
				index->blocks[0].start= cpu_to_le32(phys_block);
				index->blocks[0].count= cpu_to_le32(1);
					
			}
			mark_buffer_dirty(bh_index);
        }

        bh_data = sb_bread(sb, phys_block);
        if (!bh_data) {
            ret = -EIO;
            goto brelse_index;
        }

        if (new_block) {
            memset(bh_data->b_data, 0, OUICHEFS_BLOCK_SIZE);
        }

        if (copy_from_user(bh_data->b_data + offset_in_block, buf + total_written, available_in_block)) {
            brelse(bh_data);
            ret = -EFAULT;
            goto brelse_index;
        }

        mark_buffer_dirty(bh_data);
        sync_dirty_buffer(bh_data);
        brelse(bh_data);

        new_pos += available_in_block;
        total_written += available_in_block;
        len -= available_in_block;

        if (new_pos > inode->i_size) 
            inode->i_size = new_pos;
    }

    if (total_written > 0) {
        /* Ici on garde i_size_read juste pour le roundup, c'est plus sûr pour le calcul de blocs */
        inode->i_blocks = (roundup(inode->i_size, OUICHEFS_BLOCK_SIZE) / 
                           OUICHEFS_BLOCK_SIZE) + 1;
        inode->i_mtime = inode->i_ctime = current_time(inode);
        mark_inode_dirty(inode);
    }

    *pos = new_pos;
    ret = total_written;

brelse_index:
    if (bh_index) {
        sync_dirty_buffer(bh_index);
        brelse(bh_index);
    }

out_unlock:
    inode_unlock(inode);
    
    return ret;
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
			ret = start + logical_block;
			break;
		}
	}
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
				if(index->blocks[i].count == 0){
					break;
				}
				nb_extents++;
			}
			
			//Afichage du header
			pr_info("extents for inode %lu: %d extent(s)\n", inode->i_ino, nb_extents);

			//Affichage des extents
			for (i = 0; i < nb_extents; i++) {
				uint32_t start = index->blocks[i].start;
            	uint32_t count = index->blocks[i].count;
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
	.llseek = generic_file_llseek,
	.read = ouichefs_read,
	.write = ouichefs_write,
	.fsync = generic_file_fsync,
	.unlocked_ioctl = ouichefs_ioctl,
};

