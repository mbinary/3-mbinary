/*
#########################################################################
# File : oshfs.c
# Author: mbinary
# Mail: zhuheqin1@gmail.com
# Blog: https://mbinary.github.io
# Github: https://github.com/mbinary
# Created Time: 2018-05-02  23:09
# Description: 
#########################################################################
*/

#define FUSE_USE_VERSION 26
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <fuse.h>
#include <sys/mman.h>

#define NAME_LENGTH 255
#define BLOCKSIZE 4096
#define BLOCKNR  1048576 //1<<20 =  total/blocksize = 4g/4k

static void *mem[ BLOCKNR];
static struct filenode *root = NULL;
static int used_block = 0,last_block=0;

typedef int block_no_t;

struct filenode {
	block_no_t block_no ;
    int  content; // the value of the first block_no of data
    struct stat st;
    struct filenode *next;
    char name[NAME_LENGTH];
};

static int content_size = BLOCKSIZE - sizeof(block_no_t);
static int node_size = sizeof(struct  filenode);

/* metadata of node;  including pointer and value
 * pointer: content, pointing to data blocks;  next, pointing to next node 
 * value  : block_no, content of st,  content of name 
 */


static struct filenode *get_filenode(const char *name,struct filenode **prt)
{ 
	if( !root)return NULL;
	if(strcmp(root->name,name+1)==0){
		*prt= NULL;
		return root;
	}
    struct filenode *node = root;
    while(node->next) {
        if(strcmp(node->next->name, name + 1) != 0)
            node = node->next;
        else{
			*prt = node;
            return node->next;
		}
    }
    return NULL;
}

int map_mem(int i){
  /* get a vacant block and mmap, and init header info*/
	for(i=i%BLOCKNR;i<BLOCKNR;++i){
		if(!mem[i]){
			last_block = i;
			used_block +=1;
			mem[i] = mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			*(block_no_t*) mem[i] = -1;
			return i;
		}
	}
	return -1;
}
static struct filenode* create_filenode(const char *name, const struct stat *st)
{
	int i = map_mem(last_block);
	if(i==-1) return NULL;
	int namelen = sizeof(name);
	if(namelen>NAME_LENGTH){
		printf("Error: the length of name is beyond %d chars\n%s\n",NAME_LENGTH,name);
		return NULL;
	}
	struct filenode * nd = (struct filenode*)mem[i];
    memcpy(nd->name, name,namelen + 1); // skip the leading '/'
    memcpy(nd->st , st, sizeof(struct stat));
	nd->block_no = i;  //block_no
	nd->content=-1; //content pointer
	nd->next =root; //next pointer
    root = nd;
	return root;
}

static void *zfs_init(struct fuse_conn_info *conn)
{       
	memset(mem,0,sizeof(void *) * BLOCKNR);
	zfs_mkdir("/",0755); // to do
    return NULL;
}

static int zfs_mknod(const char *path, mode_t mode, dev_t dev)
{  
	/* when createing a file, if create not impl, call mknod  @fuse.h L368 ,106
	 * this is called for creation of all  non-directory, non-symlink nodes.
     * */
    struct stat st;
    st.st_mode =mode| S_IFREG | 0644;
    st.st_uid = fuse_get_context()->uid;
    st.st_gid = fuse_get_context()->gid;
    st.st_nlink = 1;
    st.st_size = 0;
	st.st_blocksize = BLOCKSIZE;
	st.st_atime = time(NULL);
	st.st_mtime = time(NULL);
	st.st_ctime = time(NULL);
	struct filenode * nd = create_filenode(path + 1, &st);
	if(nd==NULL) return -ENOSPC;
    return 0;
}

static int zfs_open(const char *path, struct fuse_file_info *fi)
{

    return 0;
}


static int zfs_getattr(const char *path, struct stat *stbuf)
{
    struct filenode *node = get_filenode(path,NULL);
    if(node) {
        memcpy(stbuf,&node->st, sizeof(struct stat));
		return 0;
    } 
	else return  -ENOENT;
}

static int zfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    struct filenode *node = root;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    while(node) {
        filler(buf, &node->name, &node->st, 0);
        node = node->next;
    }
    return 0;
}

void * seek_mem(block_no_t* block_no,off_t *offset){
	/*  find the begin block to read according to offset */
	block_no_t former_block = * block_no;
	while(*offset>content_size){
		former_block = *block_no;
		*block_no = *(block_no_t*) mem[*block_no];
		*offset -= content_size;
	}
	/* when writing, seeking offset, if there is no mem allocated , then allocate, 
	 * IN func zfs_write, it ensure the block_no won't be the filenode->content 
	 **/
	if(*block_no ==-1){
		*block_no = map_mem(last_block);
		if(*block_no==-1) return NULL;
		*(block_no_t*) mem[former_block] = *block_no;
	}
	return (char*)mem[*block_no] + *offset+ sizeof(block_no_t);
}

void * get_next_block(block_no_t* block_no){
	*block_no = *(block_no_t*) mem[*block_no];
	return mem[*block_no]+sizeof(block_no_t);
}
static int zfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct filenode *node = get_filenode(path,NULL);
	node->st.atime = time(NULL);
	if(offset>node->st.st_size) return -ENOENT; 
    int ret = size;
    if(offset + size> node->st->st_size)
        ret = node->st->st_size - offset;
	block_no_t tmp= node->content, *block_no= &tmp;
	void * p_mem = seek_mem(block_no,&offset);
	// new offset is within cur block 
	int cur_block_left = content_size - offset;
    memcpy(buf, (char*)p_mem, cur_block_left);
	buf+=cur_block_left;
	int left = ret- cur_block_left;
	while(left>content_size){
		memcpy(buf,(char*)get_next_block(block_no),content_size);
		buf+=content_size;
		left-=content_size;
	}
	memcpy(buf,(char*)get_next_block(block_no),left);
    return ret;
}


static int zfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct filenode *node = get_filenode(path,NULL);
	if(offset>node->st.st_size) return -ENOENT;
	if((size - node->st.st_size)/BLOCKSIZE > (BLOCKNR-used_block)) return -ENOSPC;
    node->st.st_size = offset + size;
	node->st.mtime = time(NULL);
	node->st.atime = time(NULL);
	if(node->content==-1) { // first write 
		node->content = map_mem(last_block);
		if(node->content==-1)return -ENOSPC;
	}
	block_no_t tmp  = node->content, *block_no = &tmp;
	void * p_mem  = seek_mem(block_no,&offset);
	/* after calling seek_mem, now offset is within one block*/
	node->st.blocks += (offset+size)/content_size;  //think twice
	int cur_block_left = content_size - offset;
    if(size<cur_block_left){
		memcpy(p_mem, buf, size);
		return size;
	}
	memcpy(p_mem, buf, cur_block_left);
	buf+= cur_block_left;
	int left = size-cur_block_left ;
	while(left>content_size){
		// new block 
		*(block_no_t*) mem[*block_no]  = map_mem(last_block);
		memcpy((char*)get_next_block(block_no),buf,content_size);
		buf+=content_size;
		left-=content_size;
	}
	*(block_no_t*) mem[*block_no]  = map_mem(last_block);
	memcpy((char*)get_next_block(block_no),buf,left);
    return size;
}

static int zfs_truncate(const char *path, off_t size)
{      // resize   file
    struct filenode *node = get_filenode(path,NULL);
    if(size > node->st.st_size) return 0;
	node->st.st_size = size;
	block_no_t tmp = node->content, *block_no = &tmp ;
	seek_mem(block_no,&size); // size is the offset 
	/* free exceeded blocks */
	tmp = *(block_no_t*) mem[*block_no];
	unmap_chain_block(tmp);
    return 0;
}
void unmap_chain_block(block_no_t no){
	block_no_t suc_block ;
	while(no!=-1){
		suc_block = *(block_no_t*) mem[no];
		unmap_mem(no);
		no = suc_block;
	}
}

static int zfs_unlink(const char *path)
{ 
    struct filenode *node,*prt,**prt = &node;
	node = get_filenode(path,prt);
	if(node==NULL) return -ENOENT;
	if(*prt==NULL){
		root = root->next;
	}else (*prt)->next = node->next;
	unmap_chain_block(node->content);
	return 0;
}

void unmap_mem(  int i){ 
	munmap(  mem[i],blocksize);
	mem[i] = NULL;
}
static int zfs_mkdir(const char * path, mode_t mode){
	/*to do : st_nlink */
	struct stat  st;
	st.st_uid = getuid();
	st.st_gid = getgid();
	st.st_mode = mode| S_IFDIR | 0755;
	st.st_nlink = 2;
	st.st_blocksize = BLOCKSIZE;
	st.st_atime = time(NULL);
	st.st_mtime = time(NULL);
	st.st_ctime = time(NULL);
	st.st_size = 0;
	struct filenode * nd  = create_filenode(path+1,&st);
	if(nd==NULL) return -ENOSPC;
	return 0;
	
}

static int zfs_rmdir(const char * path){
	return zfs_unlink(path);	
}

static const struct fuse_operations op = {
    .init = zfs_init,
    .getattr = zfs_getattr,
    .readdir = zfs_readdir, 
    .mknod = zfs_mknod,  
    .open = zfs_open,
    .write = zfs_write,
    .truncate = zfs_truncate,
    .read = zfs_read,
    .unlink = zfs_unlink,
	.mkdir = zfs_mkdir,
	.rmdir = zfs_rmdir,
};

int main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &op, NULL);
}
