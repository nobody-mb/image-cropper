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

struct queue {
	struct node {
		char *item;
		struct node *next;
	} *head, *tail;
	
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
	memcpy(ptr->flat, buffer, ptr->y * ptr->x);

    	stbi_image_free(buffer);

	fprintf(stderr, "alloc image %s (%d x %d) pixsz = %d\n", 
		fname, ptr->x, ptr->y, ptr->pixsz);
		
	return 0;
}

int find_x_boundary (int x1, int pixsz, int img_x, unsigned char *cmp_ptr)
{
	unsigned char *cmp_start = cmp_ptr;
#ifdef X86_64_SUPPORTED
	asm volatile (	"imulq %%rbx, %%rax\n"
			"movq %%rcx, %%rdx\n"	/* img_x */
		"fxb_loop1:\n" 
			"cmpq %%rax, %%rdx\n"
			"jl fxb_endloop\n"
			"movq %%rbx, %%rcx\n"
			"cld\n"
			"repe cmpsb\n"
			"jnz fxb_endloop\n"
			"subq %%rbx, %%rsi\n"
			"addq %%rbx, %%rax\n"
			"jmp fxb_loop1\n"
		"fxb_endloop:\n" 
			: "=a" (x1)
			: "a" (x1), "b" (pixsz), "c" (img_x),
				"S" (cmp_start), "D" (cmp_ptr)
				: "rdx");
#else
	int i;
	
	for (x1 *= pixsz; x1 < img_x; x1 += pixsz) {
		for (i = 0; i < pixsz; i++)
			if (cmp_ptr[i] != cmp_start[i]) 
				break;
		
		if (i < pixsz)
			break;
		
		cmp_ptr += pixsz;
	}
#endif

	return x1 / pixsz;
}

int find_most_common (int y1, int pixsz, int img_y, unsigned char *column_buffer)
{
#ifdef X86_64_SUPPORTED
	int max = 0;
	int num = (img_y - y1);
	
	asm volatile (		"movq %%rcx, %%r10\n"		/* pixsz */
				"xorq %%r11, %%r11\n"		/* max */
				"movq %%rsi, %%rdi\n"		/* cmp_ptr */
				"xorq %%r13, %%r13\n"		/* i */
				"xorq %%r15, %%r15\n"		/* num  */
				"xorq %%r8, %%r8\n"		/* max_pos */
			"fmc_mainloop:\n"
				"cmpq %%r13, %%rbx\n"	/* i, num */
				"jl fmc_endloop\n"
				"xorq %%rax, %%rax\n"	/* tmp_pos = 0 */
				"movq %%rbx, %%r15\n"	/* num1 = num */
				"movq %%rsi, %%rdi\n"	/* cmp_ptr = cmp_start */
				"imulq %%r10, %%r15\n"	/* num1 *= pixsz */
				"addq %%rcx, %%rdi\n"	/* cmp_ptr += num1 */
				"movq %%rbx, %%r15\n"	/* num1 = num */
			"fmc_numloop:\n"
				"decq %%r15\n"		/* num1-- */
				"jz fmc_numend\n"
				"subq %%r10, %%rdi\n"	/* cmp_ptr -= pixsz */
				"xorq %%r14, %%r14\n"	/* j = 0 */
			"fmc_pixloop:\n"
				"cmpq %%r14, %%r10\n"	/* j, pixsz */
				"jle fmc_pixend\n"
				"movb (%%rsi, %%r14), %%cl\n"	
				"movb (%%rdi, %%r14), %%dl\n"
				"cmpb %%cl, %%dl\n"	/* cmpptr[j] == cmpstart[j] */
				"jne fmc_pixend\n"
				"incq %%r14\n"		/* ++j */
				"jmp fmc_pixloop\n"
			"fmc_pixend:\n"	
				"cmpq %%r14, %%r10\n"	/* j == pixsz */
				"jne fmc_numloop\n"
				"incq %%rax\n"		/* ++tmp_pos */
				"jmp fmc_numloop\n"
			"fmc_numend:\n"
				"cmpq %%rax, %%r8\n"	/* tmp_pos, max_pos */
				"jg fmc_not_found\n"
				"movq %%r13, %%r11\n"	/* max = i */
				"movq %%rax, %%r8\n"	/* max_pos = tmp_pos */
			"fmc_not_found:\n"
				"addq %%r10, %%rsi\n"	/* cmp_start += pixsz */
				"incq %%r13\n"		/* i++ */
				"jmp fmc_mainloop\n"
			"fmc_endloop:\n"	
				"movq %%r11, %%rax\n"	/* max = max */
			: "=a" (max) : 
				"b" (num),
				"c" (pixsz),
				"S" (column_buffer)
			: "rax", "r8", "r9", "r10", "r11", "r13", 
				"r14", "r15", "rdi", "rdx");
	
	return max;
#else
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
#endif
}

int find_last_mc (unsigned char *cmp_start, unsigned char *cmp_ptr, int img_x, 
			int img_y, int y1, int pixsz)
{
#ifdef X86_64_SUPPORTED
	int retn = 0;

	asm volatile (		"movq %%rcx, %%r10\n"		/* img_x */
			"flm_loop:\n"
				"cmpq %%rdx, %%rax\n"
				"jg flm_exit\n"
				"subq %%r10, %%rdi\n"
				"decq %%rdx\n"
				"movq %%rbx, %%rcx\n"
				"cld\n"
				"repe cmpsb\n"
				"subq %%rbx, %%rsi\n"
				"subq %%rbx, %%rdi\n"
				"addq %%rcx, %%rdi\n"
				"addq %%rcx, %%rsi\n"
				"cmpq $0, %%rcx\n"
				"jne flm_loop\n"
			"flm_exit:\n"
			: "=d" (retn) : "a" (y1),
				"b" (pixsz),
				"c" (img_x),
				"d" (img_y),
				"S" (cmp_start),
				"D" (cmp_ptr)
			: "r8", "r9", "r10", "r11", "r12");
			
	return (retn <= y1) ? (img_y - 1) : retn;
#else
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
#endif
}

void build_col_buffer (int y1, int img_x, int img_y, int pixsz, 
			unsigned char *column_ptr, unsigned char *cmp_ptr)
{
#ifdef X86_64_SUPPORTED
	asm volatile (  "subq %%rcx, %%rax\n"
			"subq %%rdx, %%rbx\n"
		"bcb_loop:\n"
			"movq %%rdx, %%rcx\n"
			"cld\n"
			"rep movsb\n"
			"addq %%rbx, %%rsi\n"
			"decq %%rax\n"
			"jnz bcb_loop\n"
		:: "c" (y1), "b" (img_x), "a" (img_y), "d" (pixsz), 
			"D" (column_ptr), "S" (cmp_ptr));
#else
	int i; 
	
	for (; y1 < img_y; y1++) {
		for (i = 0; i < pixsz; i++)
			*column_ptr++ = cmp_ptr[i];

		cmp_ptr += img_x;
	}
#endif
}

int detect_br (unsigned int *x1p, unsigned int *y1p, int topleft_x, int img_x, 
	       int img_y, int pixsz, int tl_pos, unsigned char *flat)
{
	unsigned char *column_buffer, *sptr;
	int x0, y0, x1, max = 0;
	
	if (!x1p || !y1p)
		return -1;
	
	y0 = (tl_pos / img_x) - 1;
	x0 = (topleft_x + (tl_pos % img_x)) / pixsz;
	
	sptr = flat + (img_x * y0) + (x0 * pixsz);

	x1 = find_x_boundary(x0, pixsz, img_x, sptr);
	x0 -= (topleft_x / pixsz);
	
	column_buffer = calloc((img_y - y0 + 1), pixsz);

	build_col_buffer (y0, img_x, img_y, pixsz, column_buffer, sptr);
	
	max = find_most_common(y0, pixsz, img_y, column_buffer);

	*y1p = find_last_mc(column_buffer + (max * pixsz), flat + (img_y * img_x) + 
			(x0 * pixsz), img_x, img_y, y0, pixsz);
	*x1p = x1;
	
	free(column_buffer);

	return 0;
}

//output (rdi), input (rsi), start (rdx), end (rcx)

int crop_window (const char *name, struct img_dt *topleft, int num_tls, 
		struct img_dt btmright)
{
	struct img_dt img;
	int retn = -1;
	
	if (alloc_img_from_file(name, &img, topleft[0].pixsz))
		return -1;
	
	char wpath[1024];
	memset(wpath, 0, sizeof(wpath));
	
	int tl_pos = 0, br_pos = 0;
	unsigned int x0, y0, x1, y1;
	
	int i;
	
	for (i = 0; i < num_tls; i++) {
		if ((tl_pos = cmp_img(&img, &topleft[i], 256)) <= 0)
			continue;
			
		x0 = (tl_pos % img.x) / img.pixsz;
		y1 = y0 = (tl_pos / img.x);
		
		if ((br_pos = cmp_img(&img, &btmright, 256)) > 0) {
			y1 = (br_pos / img.x) + btmright.y;
			x1 = ((br_pos % img.x) + btmright.x) / img.pixsz;
		} else {
			detect_br(&x1, &y1, topleft[i].x, img.x, img.y, img.pixsz, 
					tl_pos, img.flat);
		} 
		
		printf("%s: (%d, %d) -> (%d, %d)\n", (br_pos > 0) ? "found" : "detected", 
							x0, y0, x1, y1);
		
		snprintf(wpath, sizeof(wpath), "%sc.png", name);
		retn = crop_and_write(img.x / img.pixsz, img.y, img.pixsz, img.flat, 
					x0, y0, x1, y1, wpath);
	}
	
	free(img.flat);
	
	return retn;
}

/* looks for the image "comp" in the .pngs found in "src_path", 
   copies pngs to directory "dst" if found (needs to be same format) */
int run_crop (const char *src_path, char **tl_paths, int num_tl, const char *br_path)
{
	struct dirent *ds;
	struct stat st;
	char wpath[1024];
	DIR *dr;
	int i;
	
	struct img_dt *topleft, btmright;
	
	topleft = calloc(sizeof(struct img_dt), num_tl);
	memset(&btmright, 0, sizeof(struct img_dt));
	
	for (i = 0; i < num_tl; i++)
		if (alloc_img_from_file(tl_paths[i], &topleft[i], 0))
			return -1;
			
	if (alloc_img_from_file(br_path, &btmright, 0))
		return -1;

	if ((dr = opendir(src_path)) == NULL)
		return -1;
	
	while ((ds = readdir(dr))) {
		if (*(ds->d_name) == '.')
			continue;
			
		memset(wpath, 0, sizeof(wpath));
		snprintf(wpath, sizeof(wpath), "%s/%s", src_path, ds->d_name);
			
		if (stat(wpath, &st) < 0 || S_ISDIR(st.st_mode) || 
			crop_window(wpath, topleft, num_tl, btmright))
			continue;
			
		fprintf(stderr, "found matching file %s\n", ds->d_name);
	}
	
	closedir(dr);
	
	free(btmright.flat);

	for (i = 0; i < num_tl; i++)
		free(topleft[i].flat);

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
	const char *br_path = "/Users/nobody1/Desktop/ref2/btmright.png";
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
	
	if (run_crop(src, refs, rsize, br_path) < 0)
		fprintf(stderr, "crop error\n");
	
	int i;
	for (i = 0; i < rsize; i++) {
		printf("%s\n", refs[i]);
		free(refs[i]);
	}
	
	free(refs);

	return 0;
}

