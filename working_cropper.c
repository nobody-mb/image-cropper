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

/****************************************************************************************/
/****************************************************************************************/

#ifdef FNS_UTIL
{
#endif

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

int alloc_img_from_file (const char *fname, struct img_dt *ptr, int expect_size)
{
	unsigned char *buffer = stbi_load(fname, &ptr->x, &ptr->y, &ptr->pixsz, 0);
	
	if (!buffer)
		return -1;
	
	ptr->x *= ptr->pixsz;
	
	ptr->flat = malloc(ptr->y * ptr->x);
	memcpy(ptr->flat, buffer, ptr->y * ptr->x);

    	stbi_image_free(buffer);

	fprintf(stderr, "\t[%s]: alloc image (%d x %d) (pixsz = %d):\n\t\t%s\n", 
		__func__, ptr->x, ptr->y, ptr->pixsz, fname);
		
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
	
	src_ptr += (y0 * src_x) + x0;
	
#ifdef X86_64_SUPPORTED1
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
int cmp_img (struct img_dt *img, struct img_dt *cmp, int magic)
{
	int img_len = img->x * img->y;
	int xl = img->x - cmp->x;
	int i, img_xpos = 0;
	
	for (i = 0; i < img_len; i++) {
		int cy = cmp->y;
#ifdef X86_64_SUPPORTED1
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
			        "d" (img->x - cmp->x),
			        "a" (cmp->x), "b" (cmp->y));
#else
		unsigned char *cmp_ptr = cmp->flat;
		unsigned char *img_ptr = img->flat + i;

		while (cy-- && !memcmp(cmp_ptr, img_ptr, cmp->x)) {
			cmp_ptr += cmp->x;
			img_ptr += img->x;
		}
#endif		
		if (cy <= 0) 
			return i;

		if ((img_xpos++) >= xl) {
			img_xpos = 0;
			i += cmp->x;
		}
	}
	
	return -1;
}	

/* find last instance of cmp_ptr[pixsz] in cmp_start */
int find_last_mc (unsigned char *cmp_start, unsigned char *cmp_ptr, int img_x, 
			int img_y, int y1, int pixsz)
{
#ifdef X86_64_SUPPORTED11
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

/* copy a column from a flat array into a row */
void build_col_buffer (int y1, int img_x, int img_y, int pixsz, 
			unsigned char *column_ptr, unsigned char *cmp_ptr)
{
#ifdef X86_64_SUPPORTED1
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

/* finds the last instance of the pixel at cmp_ptr in its parent row */
int find_x_boundary (int x1, int pixsz, int img_x, unsigned char *cmp_ptr)
{
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

	return x1 / pixsz;
}

/* finds the index of the most common pixsz-sized chunk in column_buffer */
int find_most_common (int y1, int pixsz, int img_y, unsigned char *column_buffer)
{
	int max = 0;
	int num = (img_y -= y1);
	
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
		num = img_y;
		
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
#endif

	return max;
}

/* attempts to detect location of the "opposite" corner from the window at tl_pos */
int detect_br (unsigned int *x1p, unsigned int *y1p, int topleft_x, int img_x, 
	       int img_y, int pixsz, int tl_pos, unsigned char *flat)
{
	unsigned char column_buffer[img_y * pixsz + 1], *tmp;
	int x0, y0, max = 0;
	unsigned long start, end;

	if (!x1p || !y1p)
		return -1;
	
	x0 = (tl_pos % img_x);
	y0 = (tl_pos / img_x);
	tmp = flat + tl_pos + topleft_x - pixsz; 
	fprintf(stderr, "\t[%s]: cmp val: #%.02X%.02X%.02X\n", 
		__func__, tmp[0], tmp[1], tmp[2]);
	
	fprintf(stderr, "\t[%s]: starting at (%d, %d)\n", __func__, x0, y0);

	start = getTime();
	*x1p = find_x_boundary(x0 + topleft_x - pixsz, pixsz, img_x, tmp);
	end = getTime();
	fprintf(stderr, "\t[%s]: found x1 = %d (%ld)\n", __func__, *x1p, (end - start));
	
	build_col_buffer(y0, img_x, img_y, pixsz, column_buffer, flat + tl_pos);
	
	start = getTime();
	max = find_most_common(y0, pixsz, img_y, column_buffer);
	end = getTime();
	tmp = column_buffer + (max * pixsz);
	fprintf(stderr, "\t[%s]: most common at %d: #%.02X%.02X%.02X (%ld)\n", 
		__func__, max, tmp[0], tmp[1], tmp[2], (end - start));
		 
	start = getTime();
	*y1p = find_last_mc(tmp, flat + (img_y * img_x) + (x0), img_x, img_y, y0, pixsz);
	end = getTime();
	fprintf(stderr, "\t[%s]: found y1 = %d (%ld)\n", __func__, *y1p, (end - start));

	return 0;
}

/* attempts to crop an image given reference images to search for, returns 0 on success */
int crop_window (const char *name, struct img_dt *tl, int num_tls, struct img_dt btmright)
{
	int tl_pos = 0, i = 0;
	unsigned int x0, y0, x1, y1;
	struct img_dt img;
	unsigned long long start, end;
	char wp[1024];
	
	if (alloc_img_from_file(name, &img, tl[0].pixsz))
		return -1;

	memset(wp, 0, sizeof(wp));

	do {
		start = getTime();
		tl_pos = cmp_img(&img, &tl[i], MAGIC * tl[i].x);
		end = getTime();
		fprintf(stderr, "[%s]: cmp returned %d for ref #%d (%lld ns)\n", 
			__func__, tl_pos, i, (end - start));
	} while (++i < num_tls && tl_pos <= 0);
	
	if (tl_pos <= 0) {
		fprintf(stderr, "[%s]: no matches found\n", __func__);
		return -1;
	}/*if ((br_pos = cmp_img(&img, &btmright, MAGIC)) > 0) {
			y0 = (tl_pos / img.x);
			x0 = (tl_pos % img.x) / img.pixsz;
			
			y1 = (br_pos / img.x) + btmright.y;
			x1 = ((br_pos % img.x) + btmright.x) / img.pixsz;
			
			fprintf(stderr, "[%s]: found br_ref (%d -> %d): "
					"(%d, %d) -> (%d, %d)\n", 
				__func__, tl_pos, br_pos, x0, y0, x1, y1);
		} else {*/
		
	y1 = y0 = (tl_pos / img.x);
	x0 = (tl_pos % img.x) / img.pixsz;

	start = getTime();
	detect_br(&x1, &y1, tl[i - 1].x, img.x, img.y, img.pixsz, tl_pos, img.flat);
	end = getTime();
	fprintf(stderr, "[%s]: detected br (%d): (%d, %d) -> (%d, %d) (%lld ns)\n", 
		__func__, tl_pos, x0, y0, x1, y1, (end - start));			
		
	snprintf(wp, sizeof(wp), "%sc.png", name);
	start = getTime();
	crop_and_write(img.x / img.pixsz, img.y, img.pixsz, img.flat, x0, y0, x1, y1, wp);
	end = getTime();
	fprintf(stderr, "[%s]: wrote %s (%lld ns)\n", __func__, name, (end - start));

	free(img.flat);
	
	return 0;
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
	
	for (i = 0; i < num_tl; i++) {
		if (alloc_img_from_file(tl_paths[i], &topleft[i], 0)) {
			fprintf(stderr, "[%s]: couldnt open %s\n", __func__, tl_paths[i]);
			return -1;
		}
	}
			
	if (br_path && alloc_img_from_file(br_path, &btmright, 0)) {
		fprintf(stderr, "[%s]: couldnt open %s\n", __func__, br_path);
		return -1;
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
			
		if (stat(wpath, &st) >= 0 && !S_ISDIR(st.st_mode) && 
		    !crop_window(wpath, topleft, num_tl, btmright))
			fprintf(stderr, "found matching file %s\n", ds->d_name);
	}
	
	closedir(dr);
	
	free(btmright.flat);
	
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

	if (run_crop(SRC_PATH, refs, rsize, NULL) < 0)
		fprintf(stderr, "crop error\n");

	free_ref_array(refs, rsize);

	return 0;
}

