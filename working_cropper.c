#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define SRC_PATH 	"/Users/nobody1/Desktop/dir"
#define REF_PATH 	"/Users/nobody1/Desktop/ref"
#define BR_PATH 	"/Users/nobody1/Desktop/ref2/btmright.png"
#define EXTENSION 	".png"
#define MAGIC 		0
#define X86_64_SUPPORTED1
#define TIME_DEBUG

/****************************************************************************************/
/****************************************************************************************/

#ifdef FNS_UTIL
{
#endif

void print_ns (unsigned long long ns)
{
	double us = (double)ns / 1000.0f;
	double ms = (double)ns / 1000000.0f;
	double s = (double)ns / 1000000000.0f;
	
	printf("\t\t(%.06lf sec) | (%.04lf ms) | (%.02lf us) | (%lld ns)\n", 
		s, ms, us, ns);
}

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

int alloc_img (const char *fname, struct img_dt *ptr, int expect_size)
{
	unsigned char *buffer = stbi_load(fname, &ptr->x, &ptr->y, &ptr->pixsz, 0);
	
	if (!buffer)
		return -1;
	
	ptr->x *= ptr->pixsz;
	
	ptr->flat = malloc(ptr->y * ptr->x);
	memcpy(ptr->flat, buffer, ptr->y * ptr->x);

    	stbi_image_free(buffer);

	fprintf(stderr, "[%s]: read image (%d x %d) pixel size %d\n", 
		__func__, ptr->x / ptr->pixsz, ptr->y, ptr->pixsz);
		
	return 0;
}

#include <mach/mach.h>
#include <mach/clock.h>
long getTime (void)
{
	clock_serv_t clock;
	mach_timespec_t mts;
	host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &clock);
	clock_get_time(clock, &mts);
	mach_port_deallocate(mach_task_self(), clock);
	return mts.tv_nsec;
}

#ifdef FNS_UTIL
}
#endif

/* crops flat image from src_ptr -> dst_ptr */
int crop_img (unsigned char *src_ptr, unsigned int src_x, unsigned int src_y, 
	      char *dst_ptr, unsigned int x0, unsigned int y0, 
	      unsigned int x1, unsigned int y1, unsigned int bpp)
{
	if (!src_ptr || !dst_ptr || !src_x || !src_y || 
	    ((x0 *= bpp) >= (x1 *= bpp) || x1 > (src_x *= bpp)) ||
	    (y0 >= y1 || y1 > src_y))
		return -1;

#ifdef TIME_DEBUG
	unsigned long long start = getTime();
#endif
	
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
		      "c" (y1 - y0), "d" (x0 + src_x - x1));
#else
	int rows_left = y1 - y0;
	int cols_left = x1 - x0;
	
	while (rows_left) {
		memcpy(dst_ptr, src_ptr, cols_left);
		
		dst_ptr += cols_left;
		src_ptr += src_x;
		--rows_left;
	}
#endif

#ifdef TIME_DEBUG
	fprintf(stderr, "[%s]: (%lld ns) crop (%d, %d) -> (%d, %d)\n", __func__, 
		getTime() - start, x0 / bpp, y0, x1 / bpp, y1);
#endif

	return 0;
}

/* crops data and writes to path */
int crop_and_write (unsigned int img_x, unsigned int img_y, unsigned int bpp, 
		    unsigned char *data, unsigned int x0, unsigned int y0, 
		    unsigned int x1, unsigned int y1, char *path)
{
	char *cropdata = calloc(img_x * img_y * bpp, 1);
	int err = 0;
	
	if (crop_img(data, img_x, img_y, cropdata, x0, y0, x1, y1, bpp) < 0)
		err = 0;
	else 
		err = stbi_write_png(path, x1 - x0, y1 - y0, bpp, 
				     cropdata, (x1 - x0) * bpp);
	
	free(cropdata);
	
	return err;
}

/* block compare with tolerance for error */
int soft_cmp_block (unsigned char *cmp_ptr, unsigned char *img_ptr, int magic,
	       int img_x, int cmp_x, int cmp_len)
{
#ifdef X86_64_SUPPORTED
	int rval = 0;
	asm volatile ("movq	%%rcx, %%r10\n"		/* magic */
		      "movq	%%rdx, %%r11\n"		/* img_x */
		      "xorq	%%r14, %%r14\n"		/* cmp_total */
		      "xorq	%%rdx, %%rdx\n"		/* cmp_xpos */
		"cb_main_loop:\n"
		      "cmpq	%%r10, %%r14\n"
		      "jg	cb_failure\n"		/* cmp_total >= magic */
		      "decq	%%rdi\n"
		      "jz	cb_success\n"		/* cmp_len-- == 0 */
		      "xorq	%%rcx, %%rcx\n"		/* cmp_cur */
		      "movb	(%%rax, %%rdx), %%cl\n"	
		      "subb	(%%rbx, %%rdx), %%cl\n"	
		      "jge	cb_positive\n"
		      "negb 	%%cl\n"
		"cb_positive:\n"
		      "addq	%%rcx, %%r14\n"		/* r14 += ab(cmp-img) */
		      "incq	%%rdx\n"		/* cmp_xpos++ */
		      "cmpq	%%rsi, %%rdx\n" 
		      "jl	cb_main_loop\n"		/* if cmp_xp > cmp_x */
		      "addq	%%rsi, %%rax\n"		/* cmp_ptr += cmp_x */
		      "addq	%%r11, %%rbx\n"		/* img_ptr += img_x */
		      "xorq	%%rdx, %%rdx\n"		/* cmp_xpos = 0 */
		      "jmp	cb_main_loop\n"
		"cb_success:\n"
		      "xorq	%%rax, %%rax\n"
		      "jmp 	cb_end\n"
		"cb_failure:\n"
		      "movq	$-1, %%rax\n"
		"cb_end:\n"
		      : "=a" (rval)
		      : "a" (cmp_ptr),
		        "b" (img_ptr),
		        "c" (magic),
		        "d" (img_x),
		        "S" (cmp_x),
		        "D" (cmp_len)
		      : "r10", "r11", "r14");
	
	return rval < 0 ? rval : 0;
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

/* searches for cmp in img, returns position if found, -1 if not */
int cmp_img (struct img_dt *img, struct img_dt *cmp)
{
	int img_len = img->x * (img->y - cmp->y);
//	unsigned char *endptr = img->flat + img_len;
	int xl = img->x - cmp->x;
	int i, img_xpos = 0, ret = -1;
	
#ifdef TIME_DEBUG
	unsigned long long start = getTime();
#endif
	
	for (i = 0; i < img_len; i += img->pixsz) {
		int cy = cmp->y;
#ifdef X86_64_SUPPORTED
		asm volatile ("cld\n"
			"cb_loop:\n"
				"movq %%rax, %%rcx\n"
				"repe cmpsb\n"
				"jnz cb_end\n"
				"addq %%rdx, %%rdi\n"
				"decq %%rbx\n"
				"jnz cb_loop\n"
			"cb_end:\n"
			      : "=c" (cy)
			      : "S" (cmp->flat), "D" (img->flat + i),
			        "d" (xl), "a" (cmp->x), "b" (cmp->y));
#else
		unsigned char *cmp_ptr = cmp->flat;
		unsigned char *img_ptr = img->flat + i;

		while (cy-- && !memcmp(cmp_ptr, img_ptr, cmp->x)) {
			cmp_ptr += cmp->x;
			img_ptr += img->x;
		}
#endif		
		if (cy <= 0) {
			ret = i;
			break;
		}

		if ((img_xpos += img->pixsz) >= xl) {
			img_xpos = 0;
			i += cmp->x;
		}
	}
	
#ifdef TIME_DEBUG
	fprintf(stderr, "[%s]: (%lld ns) found @ %d\n", __func__, getTime() - start, ret);
#endif
	
	return ret;
}	

/* find last instance of cmp[pixsz] in src */
int find_last (unsigned char *src, unsigned char *cmp, int img_x, int img_y, int y1)
{
	int retn = 0;
#ifdef TIME_DEBUG
	unsigned long long start = getTime();
#endif

#ifdef X86_64_SUPPORTED1
	asm volatile ("flm_loop:\n"
			"cmpl %%eax, %%edx\n"
			"jg flm_exit\n"
				
			"subq %%rcx, %%rdi\n"
			"decl %%eax\n"

			"movl (%%rsi), %%ebx\n"
			"cmpl %%ebx, (%%rdi)\n"
			"jne flm_loop\n"
			"flm_exit:\n"
			: "=a" (retn) 
			: "a" (img_y), "c" (img_x), "d" (y1), "S" (src), "D" (cmp));
#else
	for (retn = img_y; --retn >= y1; ) {
		cmp_ptr -= img_x;
		if (*(uint32_t *)src == *(uint32_t *)cmp)
			break;
	} 
#endif

	retn = (retn <= y1) ? (img_y - 1) : retn;

#ifdef TIME_DEBUG
	fprintf(stderr, "[%s]: (%lld ns) y1 = %d\n", __func__, getTime() - start, retn);
#endif

	return retn;
}

/* copy a column from a flat array into a row */
void build_col_buffer (int img_x, int num_y, int pixsz, uint8_t *dst, uint8_t *src)
{
#ifdef TIME_DEBUG
	unsigned long long start = getTime();
#endif

#ifdef X86_64_SUPPORTED1
	asm volatile ("bcb_loop:\n"
			"movl (%%rsi), %%ecx\n"
			"movl %%ecx, (%%rdi)\n"
			
			"addq %%rdx, %%rdi\n"
			"addq %%rbx, %%rsi\n"
			
			"decl %%eax\n"
			"jnz bcb_loop\n" : : 
			
			"b" (img_x), "a" (num_y), "d" (pixsz), 
			"D" (dst), "S" (src));
#else
	for (; num_y--; dst += pixsz, src += img_x) 
		*(uint32_t *)dst = *(uint32_t *)src;
#endif

#ifdef TIME_DEBUG
	fprintf(stderr, "[%s]: (%lld ns)\n", __func__, getTime() - start);
#endif
}

/* finds the last instance of the pixel at cmp_ptr in its parent row */
int find_x_boundary (int x1, int pixsz, int img_x, unsigned char *cmp_ptr)
{
#ifdef TIME_DEBUG
	unsigned long long start = getTime();
#endif

#ifdef X86_64_SUPPORTED1
	asm volatile (	"cld\n"
		"fxb_loop1:\n" 
			"movq %%rbx, %%rcx\n"
			"repe cmpsb\n"
			"jnz fxb_endloop\n"
			"subq %%rbx, %%rsi\n"
			"addq %%rbx, %%rax\n"
			"cmpq %%rax, %%rdx\n"
			"jge fxb_loop1\n"
		"fxb_endloop:\n" 
			: "=a" (x1)
			: "a" (x1), "b" (pixsz), "d" (img_x),
				"S" (cmp_ptr), "D" (cmp_ptr)
				: "rcx");
#else
	unsigned char *cmp_start = cmp_ptr;
	
	for (; x1 < img_x; x1 += pixsz) {
		if (memcmp(cmp_ptr, cmp_start, pixsz))
			break;
		
		cmp_ptr += pixsz;
	}
#endif

#ifdef TIME_DEBUG
	fprintf(stderr, "[%s]: (%lld ns) x1 = %d\n", __func__, 
		getTime() - start, (x1 / pixsz));
#endif

	return x1 / pixsz;
}


/* finds the index of the most common pixsz-sized chunk in column_buffer */
int find_most_common (int y1, int pixsz, int img_y, unsigned char *column_buffer)
{
	int max = 0;
	int num = (img_y -= y1);
#ifdef TIME_DEBUG
	unsigned long long start = getTime();
#endif
	
#ifdef X86_64_SUPPORTED1
	asm volatile (		"movq %%rcx, %%r10\n"		/* pixsz */
				"xorq %%r11, %%r11\n"		/* max */
				"movq %%rsi, %%rdi\n"		/* cmp_ptr */
				"xorq %%r13, %%r13\n"		/* i */
				"xorq %%r15, %%r15\n"		/* num  */
				"xorq %%r8, %%r8\n"		/* max_pos */
			"fmc_mainloop:\n"
				"cmpq %%r13, %%rbx\n"	/* i, num */
				"jge fmc_endloop\n"
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
				"jge fmc_pixend\n"
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
			: "r8", "r9", "r10", "r11", "r13", 
				"r14", "r15", "rdi", "rdx");
#else
	int i, j, tmp_pos = 0, max_pos = 0;
	unsigned char *cmp_ptr, *cmp_start = column_buffer; 
	
	for (i = 0; i < img_y; i++) {
		tmp_pos = 0;
		cmp_ptr = column_buffer + (num * pixsz);
		
		for (num = img_y; --num; cmp_ptr -= pixsz)
			if (!memcmp(cmp_ptr, cmp_start, pixsz))
				++tmp_pos;				

		if (tmp_pos >= max_pos)
			max = i, max_pos = tmp_pos;
		
		cmp_start += pixsz;
	}
#endif

#ifdef TIME_DEBUG
	unsigned char *tmp = column_buffer + (max * pixsz);
	fprintf(stderr, "[%s]: (%lld ns) most common at %d: #%.02X%.02X%.02X\n", 
		__func__, getTime() - start, max, tmp[0], tmp[1], tmp[2]);
#endif

	return max;
}

/* attempts to crop an image given reference images to search for, returns 0 on success */
int crop_window (const char *name, struct img_dt *tl, int num_tls)
{
	int tl_pos = 0, i = 0, max = 0, top_right, px;
	unsigned int x0, y0, x1, y1;
	struct img_dt img;
	char wp[1024];

	if (alloc_img(name, &img, tl[0].pixsz))
		return -1;

	do {
		tl_pos = cmp_img(&img, &tl[i]);
	} while (++i < num_tls && tl_pos <= 0);
	
	if (tl_pos <= 0)
		return -1;
	
	px = img.pixsz;
	y1 = y0 = (tl_pos / img.x);
	x0 = (tl_pos % img.x);
	top_right = tl[i - 1].x - px;
	
	unsigned char cb[img.y * px + 1];
	
	x1 = find_x_boundary(x0 + top_right, px, img.x, img.flat + tl_pos + top_right);

	build_col_buffer(img.x, img.y - y0, px, cb, img.flat + tl_pos);
	
	max = px * find_most_common(y0, px, img.y, cb);

	y1 = find_last(cb + max, img.flat + (img.y * img.x) + x0, img.x, img.y, y0);
	
	snprintf(wp, sizeof(wp), "%sc%s", name, EXTENSION);
	crop_and_write(img.x / px, img.y, px, img.flat, x0 / px, y0, x1, y1, wp);

	free(img.flat);
	
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
	int i, ret;
	struct timeval t0, t1;
	double us_total;
	
	struct img_dt *topleft = calloc(sizeof(struct img_dt), num_tl);
	
	for (i = 0; i < num_tl; i++) {
		if (alloc_img(tl_paths[i], &topleft[i], 0)) {
			fprintf(stderr, "[%s]: couldnt open %s\n", __func__, tl_paths[i]);
			return -1;
		}
	}

	if ((dr = opendir(src_path)) == NULL) {
		fprintf(stderr, "[%s]: couldnt open %s\n", __func__, src_path);
		return -1;
	}
	
	while ((ds = readdir(dr))) {
		if (*(ds->d_name) == '.')
			continue;
			
		memset(wpath, 0, sizeof(wpath));
		snprintf(wpath, sizeof(wpath), "%s/%s", src_path, ds->d_name);
			
		if (stat(wpath, &st) >= 0 && !S_ISDIR(st.st_mode)) {
			gettimeofday(&t0, NULL);
			ret = crop_window(wpath, topleft, num_tl);
			gettimeofday(&t1, NULL);
	
			us_total = ((double)(t1.tv_usec - t0.tv_usec) / 1000000.0) + 
					(t1.tv_sec - t0.tv_sec);
					    
			fprintf(stderr, "[%s]: (%.08lf sec) %s file %s\n"
				"**************************\n", __func__, 
				us_total, !ret ? "matched" : "skipped", ds->d_name);
		}
	}
	
	closedir(dr);

	for (i = 0; i < num_tl; i++)
		free(topleft[i].flat);

	return 0;
}
	
int get_ref_array (char ***array, const char *src_path, const char *ext)
{
	struct queue que;
	int extlen;
	struct dirent *ds;
	struct stat st;
	char wpath[1024];
	DIR *dr;
	const char *ptr;
	
	memset(&que, 0, sizeof(struct queue));
	
	if (!array || !src_path || !ext || 
	    (extlen = (int)strlen(ext)) <= 0 ||
	    (dr = opendir(src_path)) == NULL) {
		fprintf(stderr, "[%s]: invalid arguments\n", __func__);
		return -1;
	}
	
	while ((ds = readdir(dr))) {
		ptr = get_last(ds->d_name, '.');
		if ((*(ds->d_name) == '.') || !ptr || strncmp(ptr, ext, extlen))
			continue;
			
		memset(wpath, 0, sizeof(wpath));
		snprintf(wpath, sizeof(wpath), "%s/%s", src_path, ds->d_name);
			
		if (stat(wpath, &st) < 0 || S_ISDIR(st.st_mode))
			continue;
			
		fprintf(stderr, "[%s]: found ref file %s\n", __func__, ds->d_name);
		push(&que, wpath);
	}
	
	closedir(dr);
	
	if ((extlen = que.size) <= 0) { 
		fprintf(stderr, "[%s]: no files found w/ext %s\n", __func__, ext);
		return -1;
	}
		
	(*array) = calloc(que.size, sizeof(char *));
	
	while (que.size) {
		ptr = pop(&que);
		(*array)[que.size] = (char *)ptr;
	}
	
	return extlen;
}

void free_ref_array (char **refs, int rsize)
{
	while (--rsize >= 0)
		free(refs[rsize]);
		
	free(refs);
}

int main (int argc, const char **argv)
{
	char **refs;
	int rsize;
	
	if ((rsize = get_ref_array(&refs, REF_PATH, EXTENSION)) <= 0)
		return -1;

	if (run_crop(SRC_PATH, refs, rsize) < 0)
		fprintf(stderr, "crop error\n");

	free_ref_array(refs, rsize);

	return 0;
}

