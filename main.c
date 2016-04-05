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
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

struct img_dt {
	int x, y, pixsz;
	unsigned char **img;
	unsigned char *flat;
};

struct node {
	char *item;
	struct node *next;
};

struct queue {
	struct node *head;
	struct node *tail;
	
	int size;
};

#define X86_64_SUPPORTED __LP64__

int crop_img (unsigned char *src_ptr, unsigned int src_x, unsigned int src_y, 
	      char *dst_ptr, unsigned int x0, unsigned int y0, 
	      unsigned int x1, unsigned int y1, unsigned int bpp)
{
	if (!src_ptr || !dst_ptr || !src_x || !src_y || 
	    ((x0 *= bpp) >= (x1 *= bpp) || x1 > (src_x *= bpp)) ||
	    (y0 >= y1 || y1 > src_y))
		return -1;
	
	src_ptr += (y0 * src_x) + x0;
	
#ifdef X86_64_SUPPORTED
	asm volatile ("movl %%ebx, %%r8d\n"	/* x1 */
		      "movl %%eax, %%r9d\n"	/* dst_xpos */
		      
		      "rows_loop:\n"
		      "cld\n"
		      "movsb\n"
		      
		      "incl %%r9d\n"
		      "cmpl %%r8d, %%r9d\n"
		      "jl rows_loop\n"
		      "addq %%rdx, %%rsi\n"
		      "movl %%eax, %%r9d\n"
		      "decl %%ecx\n"
		      "jnz rows_loop\n"
		      : : "D" (dst_ptr), "S" (src_ptr), "a" (x0), "b" (x1), 
		      "c" (y1 - y0), "d" (x0 + src_x - x1): "bl");
#else
	int dst_xpos = x0;
	int rows_left = y1 - y0;
	int x1_to_end = x0 + src_x - x1;
	
	while (rows_left) {
		*dst_ptr++ = *src_ptr++;
		
		if (++dst_xpos >= x1) {
			src_ptr += x1_to_end;
			dst_xpos = x0;
			--rows_left;
		}
	}
#endif
	return 0;
	
}


int crop_and_write (unsigned int img_x, unsigned int img_y, unsigned int bpp, 
		    unsigned char *data, unsigned int x0, unsigned int y0, 
		    unsigned int x1, unsigned int y1, char *path)
{
	char *cropdata = calloc(img_x * img_y * bpp, 1);
	int err = 0;
	
	if (crop_img(data, img_x, img_y, cropdata, x0, y0, x1, y1, bpp) < 0)
		err = -1;
	else 
		err = stbi_write_png(path, x1 - x0, y1 - y0, bpp, 
				     cropdata, (x1 - x0) * bpp);
	
	free(cropdata);
	
	return err;
}


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

int cmp_block_asm (unsigned char *cmp_ptr, unsigned char *img_ptr, int magic, 
		   int img_x, int cmp_x, int cmp_len)
{
	
	//well similarly you can use repz scasb to compare a block and then stop when a mismatch is found
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
		      "movb (%%r8), %%cl\n"
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
}

int cmp_block (unsigned char *cmp_ptr, unsigned char *img_ptr, int magic,
	       int img_x, int cmp_x, int cmp_len)
{
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
}

int cmp_img (struct img_dt *img, struct img_dt *cmp, int magic)
{
	int i;
	
	int cmp_len = cmp->x * cmp->y;
	int img_len = img->x * img->y;

	int xl = img->x - cmp->x;

	int img_xpos = 0;
	for (i = 0; i < img_len; i++) {
		if (!cmp_block_asm(cmp->flat, img->flat + i, magic, 
			       img->x, cmp->x, cmp_len))
			return i;

		if ((img_xpos++) >= xl) {
			img_xpos = 0;
			i += cmp->x;
		}
	}
	
	return -1;
}	

void alloc_image_data (unsigned char ***dst, int y, int x)
{
	*dst = malloc(sizeof(unsigned char *) * y);
	
	while (y--)
		(*dst)[y] = malloc(x * sizeof(unsigned char));
}

void destroy_image_data (unsigned char **src, int y)
{
	while (y--)
		free(src[y]);
	
	free(src); 
}

int alloc_img_from_file (const char *fname, struct img_dt *ptr, int expect_size)
{
	unsigned char *buffer;
	int i;

	buffer = stbi_load(fname, &ptr->x, &ptr->y, &ptr->pixsz, 0);
	
	ptr->x *= ptr->pixsz;
	
	ptr->flat = malloc(ptr->y * ptr->x);
	memcpy(ptr->flat, buffer, ptr->y * ptr->x);

	alloc_image_data(&ptr->img, ptr->y, ptr->x);
	
	for (i = 0; i < ptr->y; i++)
		memcpy(ptr->img[i], &buffer[i * (ptr->x)], ptr->x);
	
    stbi_image_free(buffer);

	fprintf(stderr, "alloc image %s (%d x %d) pixsz = %d\n", 
		fname, ptr->x, ptr->y, ptr->pixsz);
		
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
	
	if (stat(src_path, &st) < 0 || S_ISDIR(st.st_mode))
		return -1;
	
	if (stat(dst_path, &st) < 0 || !S_ISDIR(st.st_mode))
		return -1;

	strncpy(wpath, dst_path, sizeof(wpath) - 1);
	if ((filename = get_last(src_path, '/') + 1) == NULL)
		return -1;
	strncat(wpath, "/", sizeof(wpath) - 1);
	strncat(wpath, filename, sizeof(wpath) - 1);
	
	fprintf(stderr, "[%s]: copying: \n\t\"%s\" to: \n\t\"%s\"\n", 
		__func__, src_path, wpath);
	
	if ((src_fd = open(src_path, O_RDONLY, 0777)) < 0)
		return -1;
	
	if ((dst_fd = open(wpath, O_WRONLY|O_EXCL|O_CREAT, 0777)) < 0)
		return -1;
	
	while ((res = read(src_fd, wpath, sizeof(wpath))) > 0)
		if (write(dst_fd, wpath, res) != res)
			return -1;
	
	close(src_fd);
	close(dst_fd);
	
	return 0;
}

int check_image (const char *name, const char *dst, struct img_dt *cmp)
{
	struct img_dt img;
	int retn = -1;
	
	if (alloc_img_from_file(name, &img, cmp->pixsz))
		return -1;
	
	if (img.pixsz == cmp->pixsz && cmp_img(&img, cmp, 32) >= 0)
		if (!(retn = (int)file_copy(dst, name)))
			remove(name);
	
	free(img.flat);
	destroy_image_data(img.img, img.y);
	
	return retn;
}

/* looks for the image "comp" in the .pngs found in "src_path", 
 copies pngs to directory "dst" if found (needs to be same format) */
int run (const char *src_path, const char *comp, const char *dst)
{
	struct img_dt cmp;
	struct dirent *ds;
	struct stat st;
	char wpath[1024];
	DIR *dr;
	
	alloc_img_from_file(comp, &cmp, 0);
	
	if ((dr = opendir(src_path)) == NULL)
		return -1;
	
	while ((ds = readdir(dr))) {
		if (*(ds->d_name) != '.') {
			memset(wpath, 0, sizeof(wpath));
			snprintf(wpath, sizeof(wpath), "%s/%s", src_path, ds->d_name);
			
			if (stat(wpath, &st) >= 0 && 
			    !S_ISDIR(st.st_mode) && 
			    !check_image(wpath, dst, &cmp))
				fprintf(stderr, "found matching file %s\n", ds->d_name);
			
			
		}
	}
	
	closedir(dr);
	
	free(cmp.flat);
	destroy_image_data(cmp.img, cmp.y);
	
	return 0;
}

int find_x_boundary_asm (int x1, int pixsz, int img_x, unsigned char *cmp_ptr, unsigned char *cmp_start)
{
	x1 *= pixsz;
	
	asm volatile ("movq %%rax, %%r8\n"		/* x1 */
					"movq %%rbx, %%r9\n"	/* pixsz */
					"movq %%rcx, %%r10\n"	/* img_x */
			"fxb_loop1:\n" 
				"cmpq %%r8, %%r10\n"
				"jl fxb_endloop\n"

				"xorq %%rdx, %%rdx\n"		/* i */
			"fxb_subloop:\n"
				"cmpq %%rdx, %%r9\n"
				"jl fxb_endloop\n"
			
				"movb (%%rdi, %%rdx), %%al\n"
				"movb (%%rsi, %%rdx), %%bl\n"
				
				"incq %%rdx\n"
		
				"cmpb %%al, %%bl\n"
				"je fxb_subend\n"

				"jmp fxb_subloop\n"
			
			"fxb_subend:\n"
				"cmpq %%rdx, %%r9\n"
				"jl fxb_endloop\n"
		
				"addq %%r9, %%rdi\n"
				"addq %%r9, %%r8\n"
				
				"jmp fxb_loop1\n"
			"fxb_endloop:\n"
				"movq %%r8, %%rax\n"

				: "=a" (x1)
				: "a" (x1),
					"b" (pixsz),
					"c" (img_x),
					"S" (cmp_start),
					"D" (cmp_ptr)
				: "rdx", "r8", "r9", "r10" );

	return x1 / pixsz;
}

int find_x_boundary (int x1, int pixsz, int img_x, unsigned char *cmp_ptr, unsigned char *cmp_start)
{
#if 0
	int i;
	
	for (x1 *= pixsz; x1 < img_x; x1 += pixsz) {
		for (i = 0; i < pixsz; i++)
			if (cmp_ptr[i] != cmp_start[i]) 
				break;
		
		if (i < pixsz)
			break;
		
		cmp_ptr += pixsz;
	}

	return x1 /= pixsz;
	
#else
	return find_x_boundary_asm(x1, pixsz, img_x, cmp_ptr, cmp_start);
	
#endif
}

int find_most_common_asm (int x1, int y1, int pixsz, int img_y, unsigned char *column_buffer)
{
	int max = 0;
	int num = (img_y - y1);
	
	asm volatile ("movq %%rax, %%r8\n"		/* x1 */
				"movq %%rbx, %%r9\n"		/* num */
				"movq %%rcx, %%r10\n"		/* pixsz */
				"xorq %%r11, %%r11\n"		/* max */
				"movq %%rsi, %%r12\n"		/* cmp_start */
				"movq %%rsi, %%rdi\n"		/* cmp_ptr */
				"xorq %%r13, %%r13\n"		/* i */
				"xorq %%r15, %%r15\n"		/* num  */
				"xorq %%rbx, %%rbx\n"		/* max_pos */
				
			"fmc_mainloop:\n"
				"cmpq %%r13, %%r9\n"
				"jl fmc_endloop\n"
				
				"xorq %%rax, %%rax\n"		/* tmp_pos */
				"movq %%r9, %%r15\n"
				"movq %%rsi, %%rdi\n"
				
				"movq %%r9, %%rcx\n"
				"imulq %%r10, %%rcx\n"		/* num  pixsz */
				"addq %%rcx, %%rdi\n"
				
			"fmc_numloop:\n"
				"decq %%r15\n"
				"jz fmc_numend\n"
				
				"subq %%r10, %%rdi\n"
				
				"xorq %%r14, %%r14\n"		/* j */
			"fmc_pixloop:\n"
				"cmpq %%r14, %%r10\n"
				"jl fmc_pixend\n"
				
				"movb (%%rsi, %%r14), %%cl\n"
				"movb (%%rdi, %%r14), %%dl\n"
				"cmpb %%cl, %%dl\n"
				"jne fmc_pixend\n"
								
				"incq %%r14\n"
				"jmp fmc_pixloop\n"
			"fmc_pixend:\n"	
			
				"cmpq %%r14, %%r10\n"
				"jne fmc_numloop\n"
				"incq %%rax\n"
				"jmp fmc_numloop\n"
			
			"fmc_numend:\n"
				"cmpq %%rax, %%rbx\n"
				"jle fmc_not_found\n"
				
				"movq %%r13, %%r11\n"
				"movq %%rax, %%rbx\n"
			
			"fmc_not_found:\n"
				"addq %%r10, %%r12\n"
				
				"incq %%r13\n"
				"jmp fmc_mainloop\n"
			"fmc_endloop:\n"	
			
				"movq %%r11, %%rax\n"
			: "=a" (max) : 
				"a" (x1),
				"b" (num),
				"c" (pixsz),
				"d" (img_y),
				"S" (column_buffer)
			: "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15");
	
	return max;

}

int find_most_common (int x1, int y1, int pixsz, int img_y, unsigned char *column_buffer)
{
#if 1
	int i, j;
	int max = 0;
	int tmp_pos = 0;
	int max_pos = 0;
	int num = 0;
	unsigned char *cmp_ptr;
	unsigned char *cmp_start = column_buffer; 
	
	for (i = 0; i < img_y - y1; i++) {
		tmp_pos = 0;
		num = img_y - y1;
		
		cmp_ptr = column_buffer + (num * pixsz);
		while (--num) {
			cmp_ptr -= pixsz;
			for (j = 0; j < pixsz; j++)
				if (cmp_ptr[j] != cmp_start[j])
					break;
			
			if (j == pixsz)
				++tmp_pos;				
		}

		if (tmp_pos >= max_pos) {
			max = i;
			max_pos = tmp_pos;
		}
		
		cmp_start += pixsz;
	}
	
	return max;
#else
	return find_most_common_asm(x1, y1, pixsz, img_y, column_buffer);
#endif
}

int find_last_mc_asm (unsigned char *cmp_start, unsigned char *cmp_ptr, int img_x, int img_y, int y1, int pixsz)
{
	int retn = 0;

	asm volatile ("movq %%rax, %%r8\n"		/* y1 */
				"movq %%rbx, %%r9\n"		/* pixsz */
				"movq %%rcx, %%r10\n"		/* img_x */
				"movq %%rdx, %%r11\n"		/* img_y */
				"movq %%rdx, %%r12\n"		/* temp */
				"xorq %%rax, %%rax\n"		/* i */
				
			"flm_loop:\n"
				"cmpq %%r12, %%r8\n"
				"jg flm_exit\n"
				"cmpq %%rax, %%r9\n"
				"je flm_exit\n"
				
				"subq %%r10, %%rdi\n"
				"decq %%r12\n"
				
				"subq %%rax, %%rsi\n"
				"subq %%rax, %%rdi\n"
				
				"xorq %%rax, %%rax\n"
			"flm_subloop:\n"
				"cmpq %%rax, %%r9\n"
				"je flm_exit\n"
				"movb (%%rsi, %%rax), %%bl\n"
				"movb (%%rdi, %%rax), %%cl\n"
				"cmpb %%bl, %%cl\n"
				"jne flm_loop\n"
				"incq %%rax\n"
				"jmp flm_subloop\n"

			"flm_exit:\n"
				"movq %%r12, %%rax\n"
				
			: "=a" (retn) : "a" (y1),
				"b" (pixsz),
				"c" (img_x),
				"d" (img_y),
				"S" (cmp_start),
				"D" (cmp_ptr)
			: "r8", "r9", "r10", "r11", "r12");
			
		return (retn <= y1) ? (img_y - 1) : retn;
}

int find_last_mc (unsigned char *cmp_start, unsigned char *cmp_ptr, int img_x, int img_y, int y1, int pixsz)
{
#if 0
	int temp = img_y;
	
	int i = 0;
	
	while (temp >= y1 && i != pixsz) {
		cmp_ptr -= img_x;
		--temp;

		for (i = 0; i < pixsz; i++)
			if (cmp_start[i] != cmp_ptr[i])
				break;
	}
	
	return (temp <= y1) ? (img_y - 1) : temp;
#else 
	return find_last_mc_asm(cmp_start, cmp_ptr, img_x, img_y, y1, pixsz);
#endif
}

void build_col_buffer_asm (int y1, int img_x, int img_y, int pixsz, 
				unsigned char *column_ptr, unsigned char *cmp_ptr)
{
	asm volatile (
			"movq %%rbx, %%r8\n"			/* img_x */
			"movq %%rdx, %%r9\n"			/* pixsz */
		"bcb_loop:\n"
			"cmpq %%rax, %%rcx\n"
			"jl bcb_end\n"
			"xorq %%rdx, %%rdx\n"			/* i */
		"bcb_pixloop:\n"
			"cmpq %%rdx, %%r9\n"
			"jle bcb_pixend\n"
	
			"movb (%%rsi, %%rdx), %%bl\n"
			"movb %%bl, (%%rdi)\n"
			"incq %%rdi\n"
			"incq %%rdx\n"
			"jmp bcb_pixloop\n"
		"bcb_pixend:\n"
			"addq %%r8, %%rsi\n"
			"incq %%rax\n"
			"jmp bcb_loop\n"
		"bcb_end:\n"
			:: "a" (y1), "b" (img_x), "c" (img_y), "d" (pixsz), 
				"D" (column_ptr), "S" (cmp_ptr): "r8", "r9");
}

void build_col_buffer (int y1, int img_x, int img_y, int pixsz, unsigned char *column_ptr, unsigned char *cmp_ptr)
{
	#if 0
	int i; 
	
	for (; y1 < img_y; y1++) {
		for (i = 0; i < pixsz; i++)
			*column_ptr++ = cmp_ptr[i];

		cmp_ptr += img_x;
	}
	
	#else
	build_col_buffer_asm (y1, img_x, img_y, pixsz, column_ptr, cmp_ptr);
	#endif
}

int detect_br (unsigned int *x1p, unsigned int *y1p, int topleft_x, int img_x, 
	       int img_y, int pixsz, int tl_pos, unsigned char *flat)
{
	unsigned char *column_buffer;
	int x0, y0, x1, max = 0;
	
	if (!x1p || !y1p)
		return -1;
	
	y0 = (tl_pos / img_x);
	x0 = (topleft_x + (tl_pos % img_x)) / pixsz;

	x1 = find_x_boundary(x0, pixsz, img_x, flat + (y0 * img_x) + (x0 * pixsz), 
						flat + (y0 * img_x) + (x0 * pixsz));
	x0 -= (topleft_x / pixsz);
	
	column_buffer = calloc((img_y - y0 + 1), pixsz);

	build_col_buffer (y0, img_x, img_y, pixsz, column_buffer, 
						flat + (img_x * y0) + (x0 * pixsz));
	
	max = find_most_common(x1, y0, pixsz, img_y, column_buffer);

	*y1p = find_last_mc(column_buffer + (max * pixsz), 
						flat + (img_y * img_x) + (x0 * pixsz), 
						img_x, img_y, y0, pixsz);
	*x1p = x1;
	
	free(column_buffer);

	return 0;
}

int crop_window (const char *name, struct img_dt topleft, struct img_dt btmright)
{
	struct img_dt img;
	int retn = -1;
	
	if (alloc_img_from_file(name, &img, topleft.pixsz))
		return -1;
	
	char wpath[1024];
	memset(wpath, 0, sizeof(wpath));
	
	int tl_pos = 0, br_pos = 0;
	unsigned int x0, y0, x1, y1;
	
	if ((tl_pos = cmp_img(&img, &topleft, 256)) > 0) {
		if ((br_pos = cmp_img(&img, &btmright, 256)) > 0) {
			y0 = (tl_pos / img.x);
			x0 = (tl_pos % img.x) / img.pixsz;
			
			y1 = (br_pos / img.x) + btmright.y;
			x1 = ((br_pos % img.x) + btmright.x) / img.pixsz;
			
			printf("%d -> %d | (%d, %d) -> (%d, %d)\n", tl_pos, br_pos, x0,y0,x1,y1);
			
			snprintf(wpath, sizeof(wpath), "%sc.png", name);
			retn = crop_and_write(img.x / img.pixsz, img.y, img.pixsz, img.flat, 
					      x0, y0, x1, y1, wpath);
		} else {
			y1 = y0 = (tl_pos / img.x);
			x0 = (tl_pos % img.x) / img.pixsz;

			detect_br(&x1, &y1, topleft.x, img.x, img.y, img.pixsz, tl_pos, img.flat);
			
						
			printf("******** (%d, %d) -> (%d, %d)\n", x0, y0, x1, y1);
			
			snprintf(wpath, sizeof(wpath), "%sc.png", name);
			retn = crop_and_write(img.x / img.pixsz, img.y, img.pixsz, img.flat, 
					      x0, y0, x1, y1, wpath);
		} 
	}
	
	free(img.flat);
	destroy_image_data(img.img, img.y);
	
	return retn;
}

/* looks for the image "comp" in the .pngs found in "src_path", 
   copies pngs to directory "dst" if found (needs to be same format) */
int run_crop (const char *src_path, const char *tl_path, const char *br_path)
{
	struct dirent *ds;
	struct stat st;
	char wpath[1024];
	DIR *dr;
	
	struct img_dt topleft, btmright;
	
	memset(&topleft, 0, sizeof(struct img_dt));
	memset(&btmright, 0, sizeof(struct img_dt));
	
	if (alloc_img_from_file(tl_path, &topleft, 0))
		return -1;
	if (alloc_img_from_file(br_path, &btmright, 0))
		return -1;

	if ((dr = opendir(src_path)) == NULL)
		return -1;
	
	while ((ds = readdir(dr))) {
		if (*(ds->d_name) != '.') {
			memset(wpath, 0, sizeof(wpath));
			snprintf(wpath, sizeof(wpath), "%s/%s", src_path, ds->d_name);
			
			if (stat(wpath, &st) >= 0 && 
			    !S_ISDIR(st.st_mode) && 
			    !crop_window(wpath, topleft, btmright))
				fprintf(stderr, "found matching file %s\n", ds->d_name);
			
			
		}
	}
	
	closedir(dr);
	
	free(btmright.flat);
	destroy_image_data(btmright.img, btmright.y);
	
	free(topleft.flat);
	destroy_image_data(topleft.img, topleft.y);
	
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



int main (int argc, const char **argv)
{
	const char *src = "/Users/nobody1/Desktop/dir";
	const char *ref = "/Users/nobody1/Desktop/ref";
	const char *dst = "/Users/nobody1/Desktop/out";
	const char *cur = NULL;
	
	const char *tl_path = "/Users/nobody1/Desktop/ref/topleft.png";
	const char *br_path = "/Users/nobody1/Desktop/ref/btmright.png";
	
	return run_crop(src, tl_path, br_path);
	
	struct queue que;
	
	memset(&que, 0, sizeof(struct queue));
	
	get_refs(ref, ".png", &que);
	
	while (que.size) {
		cur = pop(&que);
		printf("%s\n", cur);
		
		if (run(src, cur, dst) < 0) {
			fprintf(stderr, "error\n");
		}
	}

	return 0;
}

