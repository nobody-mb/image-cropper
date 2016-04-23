#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdint.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

struct img_dt {
	int x, y, pixsz;
	unsigned char **img;
	unsigned char *flat;
	char *path;
};

struct queue {
	struct node {
		char *item;
		struct node *next;
	} *head, *tail;
	
	int size;
};

#define X86_64_SUPPORTED __LP64__

const char *get_last (const char *dir, unsigned char cmp)
{
	const char *start = dir;
	
	while (*dir)
		dir++;
	
	while (dir >= start && *dir != cmp)
		dir--;
	
	return (dir == start) ? NULL : dir;
}

void push (struct queue *queue, const char *src) 
{
	struct node *n = calloc(sizeof(struct node), 1);
	
	n->item = strdup(src);
	n->next = NULL;
	
	if (queue->head == NULL)
		queue->head = n;
	else
		queue->tail->next = n;
	
	queue->tail = n;
	queue->size++;
}
char *pop (struct queue *queue) 
{
	struct node *head = queue->head;
	char *item = NULL;
	
	if (queue->size <= 0)
		return item;
	
	item = head->item;
	
	queue->head = head->next;
	queue->size--;
	
	free(head);
	
	return item;
}

int cmp_block (unsigned char *cmp_ptr, unsigned char *img_ptr, int magic,
	       int img_x, int cmp_x, int cmp_len)
{
#ifdef X86_64_SUPPORTED
	int rval = 0;
	asm volatile ("movq %%rcx, %%r10\n"		/* magic */
		      "movq %%rdx, %%r11\n"		/* img_x */
		      "movq %%rsi, %%r12\n"		/* cmp_x */
		      "movq %%rdi, %%r13\n"		/* cmp_len */
		      "xorq %%r14, %%r14\n"		/* cmp_total */
		      "xorq %%r15, %%r15\n"		/* cmp_xpos */
		"main_loop:\n"
		      "cmpq %%r10, %%r14\n"
		      "jge failure\n"			/* cmp_total >= magic */
		      "decq %%r13\n"
		      "jz success\n"			/* cmp_len-- == 0 */
		      "xorq %%rcx, %%rcx\n"		/* cmp_cur */
		      "xorq %%rdx, %%rdx\n"		/* img_cur */
		      "movq %%rax, %%r8\n"
		      "addq %%r15, %%r8\n"
		      "movq %%rbx, %%r9\n"
		      "addq %%r15, %%r9\n"
		      "movb (%%r8), %%cl\n"		/* use repz scasb */
		      "movb (%%r9), %%dl\n"
		      "cmpb %%cl, %%dl\n"
		      "jg dl_larger\n"
		      "subb %%cl, %%dl\n"
		      "addq %%rdx, %%r14\n"
		      "jmp after_sub\n"
		"dl_larger:\n"
		      "subb %%dl, %%cl\n"
		      "addq %%rcx, %%r14\n"
		"after_sub:\n"
		      "incq %%r15\n"
		      "cmpq %%r12, %%r15\n" 
		      "jl main_loop\n"		/* if cmp_xpos > cmp_x */
		      "addq %%r12, %%rax\n"
		      "addq %%r11, %%rbx\n"
		      "xorq %%r15, %%r15\n"
		      "jmp main_loop\n"
		"failure:\n"
		      "movq $-1, %%rax\n"
		      "jmp end\n"
		"success:\n"
		      "xorq %%rax, %%rax\n"
		"end:\n"
		      : "=a" (rval)
		      : "a" (cmp_ptr),
		        "b" (img_ptr),
		        "c" (magic),
		        "d" (img_x),
		        "S" (cmp_x),
		        "D" (cmp_len)
		      );
	
	return rval;
#else
	int cmp_total = 0;
	int cmp_xpos = 0;
	unsigned char cmp_cur = 0;
	unsigned char img_cur = 0;

	
	while (cmp_len-- && cmp_total <= magic) {
		cmp_cur = cmp_ptr[cmp_xpos];
		img_cur = img_ptr[cmp_xpos];
		
		if (cmp_cur > img_cur)
			cmp_total += (cmp_cur - img_cur);
		else
			cmp_total += (img_cur - cmp_cur);
		
		cmp_xpos++;
		if (cmp_xpos >= cmp_x) {
			cmp_ptr += cmp_x;
			img_ptr += img_x;
			cmp_xpos = 0;
		}
	}
	
	if (cmp_total < magic)
		return 0;
	
	return -1;
#endif
}

int cmp_img (struct img_dt *img, struct img_dt *cmp, int magic)
{
	int i;
	
	int cmp_len = cmp->x * cmp->y;
	int img_len = img->x * img->y;

	int xl = img->x - cmp->x;

	int img_xpos = 0;
	for (i = 0; i < img_len; i++) {
		if (!cmp_block(cmp->flat, img->flat + i, magic, 
			       img->x, cmp->x, cmp_len))
			return i;

		if ((img_xpos++) >= xl) {
			img_xpos = 0;
			i += cmp->x;
		}
	}
	
	return -1;
}	

int alloc_img_from_file (const char *fname, struct img_dt *ptr, int expect_size)
{
	unsigned char *buffer = stbi_load(fname, &ptr->x, &ptr->y, &ptr->pixsz, 0);
	
	ptr->x *= ptr->pixsz;
	
	ptr->flat = malloc(ptr->y * ptr->x);
	ptr->path = strdup(fname);
	memcpy(ptr->flat, buffer, ptr->y * ptr->x);

    	stbi_image_free(buffer);

	fprintf(stderr, "alloc image %s (%d x %d) pixsz = %d\n", 
		fname, ptr->x, ptr->y, ptr->pixsz);
		
	return 0;
}

int get_refs (const char *src_path, const char *ext, struct queue *queue)
{
	struct dirent *ds;
	struct stat st;
	char wpath[1024];
	DIR *dr;
	const char *ptr;
	int extlen = (int)strlen(ext);
	
	if ((dr = opendir(src_path)) == NULL)
		return -1;
	
	while ((ds = readdir(dr))) {
		ptr = get_last(ds->d_name, '.');
		if ((*(ds->d_name) != '.') && ptr && !strncmp(ptr, ext, extlen)) {
			memset(wpath, 0, sizeof(wpath));
			snprintf(wpath, sizeof(wpath), "%s/%s", src_path, ds->d_name);
			
			if (stat(wpath, &st) >= 0 && !S_ISDIR(st.st_mode)) {
				fprintf(stderr, "found ref file %s\n", ds->d_name);
				push(queue, wpath);
				
			}
		}
	}
	
	closedir(dr);
	
	return 0;
}


/* copies the file "src_path" into the directory "dst_path" */
ssize_t file_copy (const char *dst_path, const char *src_path)
{
	int src_fd, dst_fd;
	ssize_t res;
	char wpath[1024];
	const char *filename;
	struct stat st;
	
	if (stat(src_path, &st) < 0 || S_ISDIR(st.st_mode)) return -1;
	
	if (stat(dst_path, &st) < 0 || !S_ISDIR(st.st_mode)) return -1;
	
	strncpy(wpath, dst_path, sizeof(wpath) - 1);
	if ((filename = get_last(src_path, '/')) == NULL || !*filename) return -1;
	
	strncat(wpath, filename, sizeof(wpath) - 1);
	
	fprintf(stderr, "[%s]: copying: \n\t\"%s\" to: \n\t\"%s\"\n", 
		__func__, src_path, wpath);
	
	if ((src_fd = open(src_path, O_RDONLY, 0777)) < 0) return -1;
	
	if ((dst_fd = open(wpath, O_WRONLY|O_EXCL|O_CREAT, 0777)) < 0) return -1;
	
	while ((res = read(src_fd, wpath, sizeof(wpath))) > 0)
		if (write(dst_fd, wpath, res) != res) return -1;
	
	close(src_fd);
	close(dst_fd);
	
	return 0;
}

int crop_window (const char *name, struct img_dt *topleft, int num_tls)
{
	struct img_dt img;
	int i;
	char *ptr;
	
	if (alloc_img_from_file(name, &img, topleft[0].pixsz))
		return -1;
	
	char wpath[1024];
	struct stat st;
	

	for (i = 0; i < num_tls; i++) {
		if (cmp_img(&img, &topleft[i], 256) >= 0) {
			memset(wpath, 0, sizeof(wpath));
			
			strncpy(wpath, topleft[i].path, sizeof(wpath));
			
			printf("matches %s (%s)\n", topleft[i].path, wpath);
			
			if ((ptr = (char *)get_last(wpath, '.')) == NULL) {
				printf("could not get ext\n");
				continue;
			}
			
			*ptr = 0;
			
			if (stat(wpath, &st)) {
				if (mkdir(wpath, 0777)) {
					printf("couldnt create folder %s\n", wpath);
					continue;
				}
				printf("creating folder %s\n", wpath);
			}
			
			if (!file_copy(wpath, name))
				remove(name);
			
		}
	}
	
	free(img.flat);
	free(img.path);
	
	return 0;
}

/* looks for the image "comp" in the .pngs found in "src_path", 
 copies pngs to directory "dst" if found (needs to be same format) */
int run_crop (const char *src_path, char **tl_paths, int num_tl)
{
	struct dirent *ds;
	struct stat st;
	char wpath[1024];
	DIR *dr;
	int i;
	
	struct img_dt *topleft;
	
	topleft = calloc(sizeof(struct img_dt), num_tl);
	
	for (i = 0; i < num_tl; i++)
		if (alloc_img_from_file(tl_paths[i], &topleft[i], 0))
			return -1;

	if ((dr = opendir(src_path)) == NULL)
		return -1;
	
	while ((ds = readdir(dr))) {
		if (*(ds->d_name) != '.') {
			memset(wpath, 0, sizeof(wpath));
			snprintf(wpath, sizeof(wpath), "%s/%s", src_path, ds->d_name);
			
			if (stat(wpath, &st) >= 0 && 
			    !S_ISDIR(st.st_mode) && 
			    !crop_window(wpath, topleft, num_tl))
				fprintf(stderr, "found matching file %s\n", ds->d_name);
		}
	}
	
	closedir(dr);

	for (i = 0; i < num_tl; i++) {
		free(topleft[i].flat);
		free(topleft[i].path);
	}
	
	return 0;
}

int main (int argc, const char **argv)
{
	const char *src = "/Users/nobody1/Desktop/dir";
	const char *ref = "/Users/nobody1/Desktop/ref";
	char *cur = NULL;
	
	struct queue que;
	
	memset(&que, 0, sizeof(struct queue));
	
	get_refs(ref, ".png", &que);
	
	if (que.size == 0)
		return -1;
		
	char **refs = calloc(que.size, sizeof(char *));
	int rsize = que.size;
	
	while (que.size) {
		cur = pop(&que);
		refs[que.size] = cur;
	}
	
	if (run_crop(src, refs, rsize) < 0)
		fprintf(stderr, "crop error\n");
	
	int i;
	for (i = 0; i < rsize; i++) {
		printf("%s\n", refs[i]);
		free(refs[i]);
	}
	
	free(refs);

	return 0;
}