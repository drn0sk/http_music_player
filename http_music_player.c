#define _GNU_SOURCE
#include <http_lib/http_server.h>
#include <string.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <magic.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/mman.h>
#include <stddef.h>
#include <pthread.h>
#include <sys/time.h>
#include <ftw.h>
#include <signal.h>
#include <linux/prctl.h>
#include <sys/prctl.h>
#include <stdalign.h>
#include <sys/random.h>
#include <getopt.h>
#include <stdint.h>

struct vars {
	pthread_rwlock_t rwlock;
	int shm_fd;
	void *shmem;
	size_t shmem_used, shmem_size;
};

static long page_size = 0;
static struct vars *shvars;

static ptrdiff_t shmalloc(size_t size, size_t align) {
	if(align && shvars->shmem_used % align) shvars->shmem_used += align - shvars->shmem_used % align;
	if(shvars->shmem_size - shvars->shmem_used < size) {
		size_t new_size = shvars->shmem_size + page_size;
		while(new_size - shvars->shmem_used < size) {
			new_size += page_size;
		}
		munmap(shvars->shmem, shvars->shmem_size);
		if(ftruncate(shvars->shm_fd, new_size) < 0) {
			return -1;
		}
		shvars->shmem_size = new_size;
		shvars->shmem = mmap(NULL, shvars->shmem_size, PROT_WRITE, MAP_SHARED, shvars->shm_fd, 0);
		if(shvars->shmem == MAP_FAILED) {
			return -1;
		}
	}
	ptrdiff_t mem = shvars->shmem_used;
	shvars->shmem_used += size;
	return mem;
}

typedef ptrdiff_t path_list;
struct path_entry {
	path_list next; // next entry or -1 if this is the last
	struct timespec ctime;
	char name[]; // null terminated string
};

// points to struct cache_entry
typedef ptrdiff_t paths_cache;
struct cache_entry {
	ptrdiff_t directory; // char *
	struct timespec last_modified;
	size_t num_entries;
	path_list dir_entries;
	paths_cache rest; // next entry or -1 if this is the last
};

struct name_entry {
	size_t id;
	char *name;
	struct timespec ctime;
};
typedef struct name_entry *name_list;

static void free_namelist(name_list ls, size_t len) {
	for(size_t i = 0; i < len; i++) {
		free(ls[i].name);
	}
	free(ls);
}

static char *namelist_get_name_by_id(name_list ls, size_t len, size_t id) {
	for(size_t i = 0; i < len; i++) {
		if(ls[i].id == id) return ls[i].name;
	}
	return NULL;
}

static size_t namelist_get_arr_ind(name_list ls, size_t len, size_t id) {
	for(size_t i = 0; i < len; i++) {
		if(ls[i].id == id) return i;
	}
	return 0; // this should never happen
}

static bool cache_add(char *dir, struct timespec t, size_t len, name_list e) {
	if(pthread_rwlock_wrlock(&shvars->rwlock)) {
		return false;
	}
	shvars->shmem = mmap(NULL, shvars->shmem_size, PROT_WRITE, MAP_SHARED, shvars->shm_fd, 0);
	if(shvars->shmem == MAP_FAILED) {
		pthread_rwlock_unlock(&shvars->rwlock);
		return false;
	}
	paths_cache p_new = shmalloc(sizeof(struct cache_entry) + strlen(dir) + 1, alignof(struct cache_entry));
	if(p_new < 0) {
		pthread_rwlock_unlock(&shvars->rwlock);
		return false;
	}
	ptrdiff_t new_dir = p_new + (ptrdiff_t)sizeof(struct cache_entry);
	strcpy(shvars->shmem + new_dir, dir);
	((struct cache_entry*)(shvars->shmem+p_new))->directory = new_dir;
	((struct cache_entry*)(shvars->shmem+p_new))->last_modified = t;
	((struct cache_entry*)(shvars->shmem+p_new))->num_entries = len;
	((struct cache_entry*)(shvars->shmem+p_new))->rest = -1;
	if(p_new) {
		paths_cache *p;
		do {
			p = &(((struct cache_entry*)(shvars->shmem))->rest);
		} while(*p >= 0);
		*p = p_new;
	}
	ptrdiff_t next_e = (void*)&((struct cache_entry*)(shvars->shmem+p_new))->dir_entries - shvars->shmem;
	for(size_t i = 0 ; i < len; i++) {
		ptrdiff_t tmp = shmalloc(sizeof(path_list) + 1 + strlen(e[i].name) + sizeof(struct timespec), alignof(struct path_entry));
		if(tmp < 0) {
			pthread_rwlock_unlock(&shvars->rwlock);
			return false;
		}
		*((path_list*)(shvars->shmem + next_e)) = tmp;
		strcpy(((struct path_entry*)(shvars->shmem + *((path_list*)(shvars->shmem + next_e))))->name, e[i].name);
		((struct path_entry*)(shvars->shmem + *((path_list*)(shvars->shmem + next_e))))->ctime = e[i].ctime;
		next_e = (void*)&((struct path_entry*)(shvars->shmem + *((path_list*)(shvars->shmem + next_e))))->next - shvars->shmem;
	}
	*((path_list*)(shvars->shmem + next_e)) = -1;
	munmap(shvars->shmem, shvars->shmem_size);
	pthread_rwlock_unlock(&shvars->rwlock);
	return true;
}

static paths_cache cache_find_entry(char *dir) {
	if(pthread_rwlock_rdlock(&shvars->rwlock)) {
		return -2; // error
	}
	shvars->shmem = mmap(NULL, shvars->shmem_size, PROT_READ, MAP_SHARED, shvars->shm_fd, 0);
	if(shvars->shmem == MAP_FAILED) {
		pthread_rwlock_unlock(&shvars->rwlock);
		return -2; // error
	}
	if(shvars->shmem_used == 0) {
		// empty cache
		munmap(shvars->shmem, shvars->shmem_size);
		pthread_rwlock_unlock(&shvars->rwlock);
		return -1;
	}
	for(paths_cache p = 0; p >= 0; p = ((struct cache_entry*)(shvars->shmem+p))->rest) {
		if(!strcmp(shvars->shmem + ((struct cache_entry*)(shvars->shmem+p))->directory, dir)) {
			munmap(shvars->shmem, shvars->shmem_size);
			pthread_rwlock_unlock(&shvars->rwlock);
			return p;
		}
	}
	munmap(shvars->shmem, shvars->shmem_size);
	pthread_rwlock_unlock(&shvars->rwlock);
	return -1; // not found
}

static bool cache_update(char *dir, struct timespec t, size_t len, name_list e) {
	paths_cache p = cache_find_entry(dir);
	if(p < -1) return false;
	if(p < 0) return cache_add(dir, t, len, e);
	if(pthread_rwlock_wrlock(&shvars->rwlock)) return false;
	shvars->shmem = mmap(NULL, shvars->shmem_size, PROT_WRITE, MAP_SHARED, shvars->shm_fd, 0);
	if(shvars->shmem == MAP_FAILED) {
		pthread_rwlock_unlock(&shvars->rwlock);
		return false;
	}
	size_t i = 0;
	path_list ents = ((struct cache_entry*)(shvars->shmem + p))->dir_entries;
	size_t space = ((struct cache_entry*)(shvars->shmem + p))->rest;
	if(space < 0) space = shvars->shmem_size;
	ptrdiff_t new_e;
	for(; i < len && ents < space; i++) {
		if(space - ents < sizeof(path_list) + 1 + strlen(e[i].name) + sizeof(struct timespec)) break;
		((struct path_entry*)(shvars->shmem + ents))->ctime = e[i].ctime;
		((struct path_entry*)(shvars->shmem + ents))->next = (void*)stpcpy(((struct path_entry*)(shvars->shmem + ents))->name, e[i].name) + 1 - shvars->shmem;
		if(i+1 >= len  || ((struct path_entry*)(shvars->shmem + ents))->next >= space) new_e = (void*)&((struct path_entry*)(shvars->shmem + ents))->next - shvars->shmem;
		ents = ((struct path_entry*)(shvars->shmem + ents))->next;
	}
	for(; i < len; i++) {
		ptrdiff_t tmp = shmalloc(sizeof(path_list) + 1 + strlen(e[i].name) + sizeof(struct timespec), alignof(struct path_entry));
		if(tmp < 0) {
			pthread_rwlock_unlock(&shvars->rwlock);
			return false;
		}
		*((path_list*)(shvars->shmem + new_e)) = tmp;
		((struct path_entry*)(shvars->shmem + *((path_list*)(shvars->shmem + new_e))))->ctime = e[i].ctime;
		strcpy(((struct path_entry*)(shvars->shmem + *((path_list*)(shvars->shmem + new_e))))->name, e[i].name);
		new_e = (void*)&((struct path_entry*)(shvars->shmem + *((path_list*)(shvars->shmem + new_e))))->next - shvars->shmem;
	}
	*((path_list*)(shvars->shmem + new_e)) = -1;
	((struct cache_entry*)(shvars->shmem+p))->last_modified = t;
	((struct cache_entry*)(shvars->shmem+p))->num_entries = len;
	munmap(shvars->shmem, shvars->shmem_size);
	pthread_rwlock_unlock(&shvars->rwlock);
	return true;
}

static size_t cache_find(char *dir, struct timespec *t, name_list *paths) {
	paths_cache p = cache_find_entry(dir);
	if(p < 0) return 0;
	if(pthread_rwlock_rdlock(&shvars->rwlock)) return 0;
	shvars->shmem = mmap(NULL, shvars->shmem_size, PROT_READ, MAP_SHARED, shvars->shm_fd, 0);
	if(shvars->shmem == MAP_FAILED) {
		pthread_rwlock_unlock(&shvars->rwlock);
		return 0;
	}
	size_t len = ((struct cache_entry*)(shvars->shmem+p))->num_entries;
	*paths = calloc(len, sizeof(struct name_entry));
	if(!*paths) {
		munmap(shvars->shmem, shvars->shmem_size);
		pthread_rwlock_unlock(&shvars->rwlock);
		return 0;
	}
	path_list e = ((struct cache_entry*)(shvars->shmem+p))->dir_entries;
	for(size_t i = 0; i < len; i++) {
		if(e <= 0) {
			free_namelist(*paths, i);
			munmap(shvars->shmem, shvars->shmem_size);
			pthread_rwlock_unlock(&shvars->rwlock);
			return 0;
		}
		(*paths)[i].id = i;
		(*paths)[i].ctime = ((struct path_entry*)(shvars->shmem + e))->ctime;
		(*paths)[i].name = strdup(((struct path_entry*)(shvars->shmem + e))->name);
		if(!(*paths)[i].name) {
			free_namelist(*paths, i);
			munmap(shvars->shmem, shvars->shmem_size);
			pthread_rwlock_unlock(&shvars->rwlock);
			return 0;
		}
		e = ((struct path_entry*)(shvars->shmem + e))->next;
	}
	*t = ((struct cache_entry*)(shvars->shmem+p))->last_modified;
	munmap(shvars->shmem, shvars->shmem_size);
	pthread_rwlock_unlock(&shvars->rwlock);
	return len;
}

static char *get_filetype(const char *fname) {
	magic_t m = magic_open(MAGIC_SYMLINK | MAGIC_MIME | MAGIC_ERROR);
	if(!m) return NULL;
	if(magic_check(m, NULL)) {
		magic_close(m);
		return NULL;
	}
	const char *tmp = magic_file(m, fname);
	if(!tmp) {
		magic_close(m);
		return NULL;
	}
	char *filetype = strdup(tmp);
	int err = errno;
	magic_close(m);
	errno = err;
	return filetype;
}

static struct timespec newer(struct timespec a, struct timespec b) {
	struct timespec res = a;
	if(b.tv_sec > a.tv_sec) res = b;
	if(a.tv_sec == b.tv_sec) {
		if(b.tv_nsec > a.tv_nsec) res = b;
	}
	return res;
}

static struct timespec nftw_gettime_val = {0};
static int nftw_gettime(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
	if(typeflag == FTW_D) {
		nftw_gettime_val = newer(nftw_gettime_val, sb->st_mtim);
	}
	return 0;
}

// any function where grow_buff(n) > n for all n >= 0
static size_t grow_buff(size_t old_len) {
	return (old_len) ? old_len + (old_len >> 2) : 5; // old_len * 1.5 if old_len > 0, otherwise 5
}

static name_list names = NULL;
static size_t names_len = 0, names_len_max = 0;
static int fn(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
	if(typeflag == FTW_F) {
		char *filetype = get_filetype(fpath);
		bool ok = (filetype && (!strncasecmp(filetype, "audio/", 6) || !strncasecmp(filetype, "video/", 6)));
		free(filetype);
		if(ok) {
			if(names_len + 1 > names_len_max) {
				size_t new_len = grow_buff(names_len_max);
				if(new_len <= names_len_max) {
					if(names_len_max) free_namelist(names, names_len);
					return -1;
				}
				void *new_names = reallocarray(names, new_len, sizeof(struct name_entry));
				if(!new_names) {
					if(names_len_max) free_namelist(names, names_len);
					return -1;
				}
				names = new_names;
				names_len_max = new_len;
			}
			char *fpath_tmp = strdup(fpath);
			if(!fpath_tmp) {
				if(names_len_max) free_namelist(names, names_len);
				return -1;
			}
			names[names_len].id = names_len;
			names[names_len].ctime = sb->st_ctim;
			names[names_len++].name = fpath_tmp;
		}
	} else if(typeflag == FTW_D) {
		nftw_gettime_val = newer(nftw_gettime_val, sb->st_ctim);
	}
	return 0;
}

static int cmp_name(const void *a, const void *b, void *ord) {
	const char *str1 = ((struct name_entry*)a)->name;
	const char *str2 = ((struct name_entry*)b)->name;
	const char *start1 = strrchr(str1, '/');
	if(!start1++) start1 = str1;
	const char *start2 = strrchr(str2, '/');
	if(!start2++) start2 = str2;
	char base1[strlen(start1) + 1];
	char base2[strlen(start2) + 1];
	strcpy(base1, start1);
	strcpy(base2, start2);
	char *end1 = strrchr(base1, '.');
	if(end1) *end1 = '\0';
	char *end2 = strrchr(base2, '.');
	if(end2) *end2 = '\0';
	int rv = strcasecmp(base1, base2);
	return *((int*)ord) * ((rv) ? rv : strcmp(base1, base2));
}

static int cmp_ctime(const void *a, const void *b, void *ord) {
	struct timespec t1 = ((struct name_entry*)a)->ctime;
	struct timespec t2 = ((struct name_entry*)b)->ctime;
	return *((int*)ord) * ((t2.tv_sec == t1.tv_sec) ? t2.tv_nsec - t1.tv_nsec : t2.tv_sec - t1.tv_sec);
}

enum CMD {
	CURRENT,
	LIST,
	// add more commands?
	INVALID
};

int logfile = -1;

#ifndef HTML_FILE
#define HTML_FILE "http_music_player.html"
#endif
char *html_file = HTML_FILE;
// set dir to null to use current directory
// reason does not have to be freed
// but content_type and content do have to be freed if they are not NULL (just passing it to free is fine even if it's NULL)
int get_response(char *target, query_list q, char *dir, headers *h, cookies c, char **reason, char **content_type, int *out_fd, size_t *content_len, struct timespec *ctime, char **etag) {
	headers req_hdrs = *h, resp_hdrs = NULL;
	*h = NULL;
	*etag = NULL;
	*reason = "Internal Error";
	char *path = NULL;
	size_t i = 0;
	int fd = -1;
	char *filetype;
	name_list paths = NULL;
	size_t paths_len;
	struct timespec t = {0};
	if(target[strlen(target) - 1] != '/') {
		enum CMD cmd = INVALID;
		char *cmd_str = target;
		if(*cmd_str == '/') cmd_str++;
		if(!strcasecmp(cmd_str, "current")) cmd = CURRENT;
		if(!strcasecmp(cmd_str, "list")) cmd = LIST;
		// add more commands?
		const char *playlist = get_cookie(c, "playlist");
		if(!playlist) {
			*reason = "Bad Request";
			return 400;
		}
		path = strdup(playlist);
		if(!path) return 500;
		if(cmd == CURRENT) {
			const char *index = get_cookie(c, "index");
			if(!index || !*index || sscanf(index, "%zu", &i) < 1) {
				free(path);
				*reason = "Bad Request";
				return 400;
			}
		}
		paths_len = cache_find(path, &t, &paths);
		if(!paths_len) {
			if(logfile >= 0) dprintf(logfile, "ERROR: uninitialized\n");
			free(path);
			*reason = "Bad Request";
			return 400;
		}
		if(cmd == LIST) {
			int order = 1;
			int (*cmp)(const void*, const void*, void*) = NULL;
			// sort=r?(name|time) (ie: name,rname,time,rtime)
			// 'r' reverses the order (default order (no 'r') for 'name' is alphabetical, for 'time' it's from newest to oldest)
			// 'name' sorts by song name
			// 'time' sorts by last modified time
			const char *sort = get_query(q, "sort");
			if(!sort) sort = "";
			if(*sort == 'r') {
				order = -1;
				sort++;
			}
			if(!*sort) {
				cmp = cmp_name;
			} else {
				if(!strcmp(sort, "name")) cmp = cmp_name;
				if(!strcmp(sort, "time")) cmp = cmp_ctime;
			}
			if(!cmp) {
				free(path);
				free_namelist(paths, paths_len);
				*reason = "Bad Request";
				return 400;
			}
			nftw_gettime_val = (const struct timespec){0};
			if(nftw(path, nftw_gettime, 5, 0) < 0) {
				free(path);
				free_namelist(paths, paths_len);
				return 500;
			}
			if(((t.tv_sec == nftw_gettime_val.tv_sec) ? nftw_gettime_val.tv_nsec > t.tv_nsec : nftw_gettime_val.tv_sec > t.tv_sec)) {
				free_namelist(paths, paths_len);
				nftw_gettime_val = (const struct timespec){0};
				names_len = 0;
				if(nftw(path, fn, 5, 0)) {
					free(path);
					*reason = "Could not list directory";
					return 500;
				}
				if(!names_len) {
					free(path);
					if(names_len_max) free_namelist(names, names_len);
					// Directory does not contain any audio files
					*reason = "Not Found";
					return 404;
				}
				paths = names;
				paths_len = names_len;
				t = nftw_gettime_val;
				if(!cache_update(path, t, paths_len, paths)) {
					free(path);
					free_namelist(paths, paths_len);
					return 500;
				}
			}
			qsort_r(paths, paths_len, sizeof(struct name_entry), cmp, &order);
		}
		if(!update_header(&resp_hdrs, strdup("Vary"), strdup("Cookie"))) {
			free(path);
			free_namelist(paths, paths_len);
			return 500;
		}
		free(path);
		path = NULL;
		if(cmd != LIST) {
			path = strdup(namelist_get_name_by_id(paths, paths_len, i));
			free_namelist(paths, paths_len);
			if(!path) {
				free_headers(resp_hdrs);
				return 500;
			}
			char *title = strdup(path);
			if(!title) {
				free(path);
				free_headers(resp_hdrs);
				return 500;
			}
			char *new_title = NULL;
			char *filename = strrchr(title, '/');
			filename = (filename) ? filename + 1 : title;
			char *dot = strrchr(filename, '.');
			if(dot) *dot = '\0';
			if(!url_encode(filename, &new_title, 0)) {
				free(title);
				free(path);
				free_headers(resp_hdrs);
				return 500;
			}
			free(title);
			title = new_title;
			char *disp = malloc(strlen(title) + sizeof("inline; filename*=UTF-8''"));
			if(!disp) {
				free(title);
				free(path);
				free_headers(resp_hdrs);
				return 500;
			}
			strcpy(disp, "inline; filename*=UTF-8''");
			strcat(disp, title);
			if(!update_header(&resp_hdrs, strdup("Content-Disposition"), disp)) {
				free(disp);
				free(title);
				free(path);
				free_headers(resp_hdrs);
				*reason = "Failed to add header";
				return 500;
			}
			free(title);
		} else {
			filetype = strdup("application/json");
			if(!filetype) {
				free_headers(resp_hdrs);
				free_namelist(paths, paths_len);
				return 500;
			}
			struct iovec iov_arr[paths_len + 2];
			struct iovec *iov = iov_arr;
			struct iovec *iov_tmp = iov+1;
			size_t total_len = 0;
			size_t idx_len = (size_t)((sizeof(size_t) * 617) >> 8) + 1;
			char idx[idx_len + 1];
			int retv;
			for(size_t k = 0; k < paths_len; k++) {
				retv = snprintf(idx, idx_len + 1, "%zu", paths[k].id) >= idx_len;
				if(retv < 0 || retv > idx_len) {
					for(size_t j = 0; j < k; j++) free(iov_tmp[j].iov_base);
					free(filetype);
					free_headers(resp_hdrs);
					free_namelist(paths, paths_len);
					return 500;
				}
				char *path = paths[k].name;
				char *start = strrchr(path, '/');
				if(!start) start = path - 1;
				start++;
				char *end = strrchr(start, '.');
				if(end) *end = '\0';
				char *tmp = malloc(strlen(start) + strlen(idx) + 18); // length of: {"id":<id>,"name":"<name>"},
				if(!tmp) {
					for(size_t j = 0; j < k; j++) free(iov_tmp[j].iov_base);
					free(filetype);
					free_headers(resp_hdrs);
					free_namelist(paths, paths_len);
					return 500;
				}
				char *tend = tmp;
				tend = stpcpy(tend, "{\"id\":");
				tend = stpcpy(tend, idx);
				tend = stpcpy(tend, ",\"name\":\"");
				tend = stpcpy(tend, start);
				tend = stpcpy(tend, "\"}");
				*tend = ',';
				iov_tmp[k].iov_base = tmp;
				iov_tmp[k].iov_len = tend - tmp;
				if(k < paths_len - 1) iov_tmp[k].iov_len++;
				total_len += iov_tmp[k].iov_len;
			}
			free_namelist(paths, paths_len);
			iov[0].iov_base = strdup("[");
			if(!iov[0].iov_base) {
				for(size_t j = 1; j < paths_len + 1; j++) free(iov[j].iov_base);
				free(filetype);
				free_headers(resp_hdrs);
				return 500;
			}
			iov[0].iov_len = 1;
			iov[paths_len + 1].iov_base = strdup("]");
			if(!iov[paths_len + 1].iov_base) {
				for(size_t j = 0; j < paths_len + 1; j++) free(iov[j].iov_base);
				free(filetype);
				free_headers(resp_hdrs);
				return 500;
			}
			iov[paths_len + 1].iov_len = 1;
			total_len += 2;
			int pipefd[2];
			if(pipe(pipefd) < 0) {
				for(size_t j = 0; j < paths_len + 2; j++) free(iov[j].iov_base);
				free(filetype);
				free_headers(resp_hdrs);
				return 500;
			}
			pid_t p = fork();
			if(p < 0) {
				close(pipefd[0]);
				close(pipefd[1]);
				for(size_t j = 0; j < paths_len + 2; j++) free(iov[j].iov_base);
				free(filetype);
				free_headers(resp_hdrs);
				return 500;
			}
			if(p) {
				close(pipefd[1]);
				for(size_t j = 0; j < paths_len + 2; j++) free(iov[j].iov_base);
			} else {
				// child
				int rv1 = prctl(PR_SET_PDEATHSIG, SIGTERM);
				free(filetype);
				free_headers(resp_hdrs);
				close(pipefd[0]);
				if(rv1 < 0) {
					close(pipefd[1]);
					for(size_t j = 0; j < paths_len + 2; j++) free(iov[j].iov_base);
					pthread_rwlock_destroy(&shvars->rwlock);
					munmap(shvars, page_size);
					shm_unlink("/shmem");
					if(logfile >= 0) close(logfile);
					exit(1);
				}
				size_t iov_size = paths_len + 2, tmp_len = total_len;
				bool partial = false;
				ssize_t rv;
				while((rv = vmsplice(pipefd[1], iov, iov_size, SPLICE_F_GIFT)) < tmp_len) {
					if(rv < 0) {
						switch(errno) {
						case EAGAIN:
						case ENOMEM:
						case EINTR:
							continue;
						}
						close(pipefd[1]);
						for(struct iovec *tmp = iov + ((partial) ? 1 : 0); tmp - iov < iov_size; tmp++) free(tmp->iov_base);
						pthread_rwlock_destroy(&shvars->rwlock);
						munmap(shvars, page_size);
						shm_unlink("/shmem");
						if(logfile >= 0) close(logfile);
						exit(1);
					}
					tmp_len -= rv;
					partial = false;
					for(struct iovec *tmp = iov; tmp - iov < iov_size; tmp++) {
						if(!rv) break;
						if(rv < tmp->iov_len) {
							tmp->iov_len -= rv;
							tmp->iov_base += rv;
							partial = true;
							break;
						} else {
							rv -= tmp->iov_len;
							iov++;
							iov_size--;
						}
					}
				}
				close(pipefd[1]);
				pthread_rwlock_destroy(&shvars->rwlock);
				munmap(shvars, page_size);
				shm_unlink("/shmem");
				if(logfile >= 0) close(logfile);
				exit(0);
			}
			fd = pipefd[0];
			*content_len = total_len;
			*ctime = t;
			*etag = NULL; // not yet implemented
		}
	} else {
		path = target;
		int dirfd;
		if(dir) {
			dirfd = open(dir, O_DIRECTORY | O_PATH);
			if(dirfd < 0) {
				*reason = "Could not open directory";
				return 500;
			}
		} else {
			dirfd = AT_FDCWD;
		}
		struct open_how how = {0};
		how.flags = O_RDONLY;
		how.resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS;
		fd = syscall(SYS_openat2, dirfd, path, &how, sizeof(how));
		if(dirfd != AT_FDCWD && close(dirfd) < 0) {
			if(fd >= 0) close(fd);
			*reason = "Could not close directory";
			return 500;
		}
		if(fd < 0) {
			switch(errno) {
			case ELOOP:
			case EXDEV:
			case EINVAL:
			case ENAMETOOLONG:
				*reason = "Bad Request";
				return 400;
			case ENODEV:
			case ENOENT:
			case ENOTDIR:
			case ENXIO:
			case EACCES:
				*reason = "Not Found";
				return 404;
			default:
				*reason = "Could not open file";
				return 500;
			}
		}
		char link[25];
		int wr = snprintf(link, 25, "/proc/self/fd/%d", fd);
		if(wr < 0 || wr >= 25) {
			close(fd);
			return 500;
		}
		struct stat sb;
		if(lstat(link, &sb) < 0) {
			close(fd);
			return 500;
		}
		size_t path_len;
		ssize_t nbytes = (sb.st_size) ? sb.st_size : PATH_MAX;
		char *full_path = NULL, *old_path = NULL;
		do {
			path_len = nbytes;
			old_path = full_path;
			full_path = realloc(full_path, path_len + 1);
			if(!full_path) {
				free(old_path);
				close(fd);
				return 500;
			}
			nbytes = readlink(link, full_path, path_len + 1);
			if(nbytes < 0) {
				free(full_path);
				close(fd);
				return 500;
			}
		} while((size_t)nbytes > path_len);
		full_path[nbytes] = '\0';
		close(fd);
		fd = -1;
		path = full_path;
		paths_len = cache_find(path, &t, &paths);
		if(paths_len) {
			nftw_gettime_val = (const struct timespec){0};
			if(nftw(path, nftw_gettime, 5, 0) < 0) {
				free(path);
				if(paths_len) free_namelist(paths, paths_len);
				return 500;
			}
		}
		if(!(paths_len && !((t.tv_sec == nftw_gettime_val.tv_sec) ? nftw_gettime_val.tv_nsec > t.tv_nsec : nftw_gettime_val.tv_sec > t.tv_sec))) {
			if(paths_len) free_namelist(paths, paths_len);
			nftw_gettime_val = (const struct timespec){0};
			names_len = 0;
			if(nftw(path, fn, 5, 0)) {
				free(path);
				*reason = "Could not list directory";
				return 500;
			}
			if(!names_len) {
				free(path);
				if(names_len_max) free_namelist(names, names_len);
				// Directory does not contain any audio files
				*reason = "Not Found";
				return 404;
			}
			paths = names;
			paths_len = names_len;
			if(!cache_update(path, nftw_gettime_val, paths_len, paths)) {
				free(path);
				free_namelist(paths, paths_len);
				return 500;
			}
		}
		free_namelist(paths, paths_len);
		char *cookie_str;
		if(asprintf(&cookie_str, "playlist=%s; Path=/", path) < 0) {
			free(path);
			return 500;
		}
		free(path);
		if(!add_header(&resp_hdrs, strdup("Set-Cookie"), cookie_str)) {
			free(cookie_str);
			return 500;
		}
		path = strdup(html_file);
		if(!path) {
			free_headers(resp_hdrs);
			return 500;
		}
	}
	if(fd < 0) {
		fd = open(path, O_RDONLY);
		if(fd < 0) {
			free(path);
			free_headers(resp_hdrs);
			return 500;
		}
		struct stat s = {0};
		if(fstat(fd, &s) < 0) {
			free(path);
			free_headers(resp_hdrs);
			close(fd);
			return 500;
		}
		char *name = path;
		char *new_name = NULL;
		if(!url_encode(name, &new_name, 0)) {
			free(name);
			free_headers(resp_hdrs);
			close(fd);
			return 500;
		}
		name = new_name;
		filetype = get_filetype(path);
		free(path);
		if(!filetype) {
			free(name);
			free_headers(resp_hdrs);
			close(fd);
			return 500;
		}
		if(asprintf(etag, "\"%s-%ld.%ld\"", name, s.st_ctim.tv_sec, s.st_ctim.tv_nsec) < 0) {
			free(filetype);
			free(name);
			free_headers(resp_hdrs);
			close(fd);
			return 500;
		}
		free(name);
		*content_len = (size_t)s.st_size;
		*ctime = s.st_ctim;
	}
	*h = resp_hdrs;
	*content_type = filetype;
	*out_fd = fd;
	*reason = "ok";
	return 200;
}

static size_t shuffle(size_t i, size_t len) {
	size_t rand_i;
	if(getrandom(&rand_i, sizeof(rand_i), 0) < sizeof(rand_i)) {
		srandom(time(NULL));
		rand_i = random();
	}
	return i + (rand_i % (len - 1)) + 1;
}

enum POST_CMD {
	START,
	NEXT,
	PREV,
	SET,
	QUEUE,
	UNQUEUE,
	// add more commands?
	INVALID_POST
};

int post_response(char *target, query_list q, char *dir, char* body, size_t body_size, headers *h, cookies c, char **reason, char **content_type, int *out_fd, size_t *content_len, struct timespec *ctime, char **etag) {
	headers req_hdrs = *h, resp_hdrs = NULL;
	*h = NULL;
	*etag = NULL;
	*reason = "Internal Error";
	const char *c_tmp;
	if(!(c_tmp = get_header(req_hdrs, "Content-Type")) || strncmp(c_tmp, "text/plain", 10)) {
		if(!update_header(&resp_hdrs, strdup("Accept"), strdup("text/plain"))) return 500;
		*reason = "Unsupported Media Type";
		return 415;
	}
	char *end = body + body_size;
	do {
		end--;
		body_size--;
	} while(isspace(*end));
	body_size++;
	while(isspace(*body)) {
		body++;
		body_size--;
	}
	enum POST_CMD cmd = INVALID_POST;
	if(!strncasecmp(body, "start", body_size)) cmd = START;
	if(!strncasecmp(body, "next", body_size)) cmd = NEXT;
	if(!strncasecmp(body, "prev", body_size)) cmd = PREV;
	if(!strncasecmp(body, "set", (body_size > 3) ? 3 : body_size)) cmd = SET;
	if(!strncasecmp(body, "queue", (body_size > 5) ? 5 : body_size)) cmd = QUEUE;
	if(!strncasecmp(body, "unqueue", (body_size > 7) ? 7 : body_size)) cmd = UNQUEUE;
	// add more commands?
	if(cmd == INVALID_POST) {
		if(logfile >= 0) dprintf(logfile, "ERROR: Invalid post command: '%.*s'\n", (int)body_size, body);
		*reason = "Bad Request";
		return 400;
	}
	const char *cpath = get_cookie(c, "playlist");
	if(!cpath) {
		*reason = "Bad request";
		return 400;
	}
	char *path = strdup(cpath);
	if(!path) {
		return 500;
	}
	char *hist = NULL;
	if(cmd == NEXT || cmd == PREV || cmd == SET) {
		const char *chist = get_cookie(c, "history");
		if(!chist) chist = "";
		hist = strdup(chist);
		if(!hist) {
			free(path);
			return 500;
		}
	}
	char *que = NULL;
	if(cmd == NEXT || cmd == QUEUE || cmd == UNQUEUE) {
		const char *cque = get_cookie(c, "queue");
		if(!cque) cque = "";
		que = strdup(cque);
		if(!que) {
			free(hist);
			free(path);
			return 500;
		}
	}
	size_t i;
	if(cmd != START) {
		const char *index = get_cookie(c, "index");
		if(!index || !*index || sscanf(index, "%zu", &i) < 1) {
			free(que);
			free(hist);
			free(path);
			*reason = "Bad Request";
			return 400;
		}
	}
	bool is_shuffle;
	int order = 1;
	int (*cmp)(const void*, const void*, void*) = NULL;
	if(cmd == START || cmd == NEXT || cmd == PREV) {
		is_shuffle = get_query(q, "shuffle");
		// sort=r?(name|time) (ie: name,rname,time,rtime)
		// 'r' reverses the order (default order (no 'r') for 'name' is alphabetical, for 'time' it's from newest to oldest)
		// 'name' sorts by song name
		// 'time' sorts by last modified time
		const char *sort = get_query(q, "sort");
		if(!sort) sort = "";
		if(*sort == 'r') {
			order = -1;
			sort++;
		}
		if(!*sort) {
			cmp = cmp_name;
		} else {
			if(!strcmp(sort, "name")) cmp = cmp_name;
			if(!strcmp(sort, "time")) cmp = cmp_ctime;
		}
		if(!cmp) {
			free(que);
			free(hist);
			free(path);
			*reason = "Bad Request";
			return 400;
		}
	}
	name_list paths = NULL;
	struct timespec t = {0};
	size_t paths_len = cache_find(path, &t, &paths);
	free(path);
	if(!paths_len) {
		free(que);
		free(hist);
		*reason = "Bad Request";
		return 400;
	}
	if(cmd == START || cmd == NEXT || cmd == PREV) qsort_r(paths, paths_len, sizeof(struct name_entry), cmp, &order);
	if(cmd == NEXT || cmd == SET) {
		char *history;
		if(asprintf(&history, "%s:%zu", hist, i) < 0) {
			free(que);
			free(hist);
			free_namelist(paths, paths_len);
			return 500;
		}
		free(hist);
		hist = history;
	}
	if(cmd == QUEUE) {
		size_t q;
		if(body[5] != '=' || sscanf(body + 6, "%zu", &q) < 1) {
			free(que);
			free(hist);
			free_namelist(paths, paths_len);
			*reason = "Bad Request";
			return 400;
		}
		char *queue;
		if(asprintf(&queue, ":%zu%s", q, que) < 0) {
			free(que);
			free(hist);
			free_namelist(paths, paths_len);
			return 500;
		}
		free(que);
		que = queue;
	}
	if(cmd == UNQUEUE) {
		char *elem = body + 7;
		if(*elem != '=') {
			free(que);
			free(hist);
			free_namelist(paths, paths_len);
			*reason = "Bad Request";
			return 400;
		}
		*elem = ':';
		char *d = elem + 1;
		do {
			if(!isdigit(*d)) {
				free(que);
				free(hist);
				free_namelist(paths, paths_len);
				*reason = "Bad Request";
				return 400;
			}
		} while(*++d);
		char *elem_loc = strstr(que, elem);
		if(!elem_loc) {
			free(que);
			free(hist);
			free_namelist(paths, paths_len);
			*reason = "Bad Request";
			return 400;
		}
		size_t elem_len = strlen(elem);
		memmove(elem_loc, elem_loc + elem_len, strlen(elem_loc + elem_len) + 1);
	}
	switch(cmd) {
	case START:
		i = paths[0].id;
		break;
	case NEXT:
		if(*que) {
			char *queue = strrchr(que, ':');
			if(!queue) {
				free(que);
				free(hist);
				free_namelist(paths, paths_len);
				*reason = "Bad Request";
				return 400;
			}
			*queue++ = '\0';
			if(sscanf(queue, "%zu", &i) < 1) {
				free(que);
				free(hist);
				free_namelist(paths, paths_len);
				*reason = "Bad Request";
				return 400;
			}
		} else {
			que = NULL;
			if(is_shuffle) {
				i = shuffle(i, paths_len);
				i %= paths_len;
			} else {
				i = namelist_get_arr_ind(paths, paths_len, i);
				i++;
				i %= paths_len;
				i = paths[i].id;
			}
		}
		break;
	case PREV:
		if(*hist) {
			char *history = strrchr(hist, ':');
			if(!history) {
				free(que);
				free(hist);
				free_namelist(paths, paths_len);
				*reason = "Bad Request";
				return 400;
			}
			*history++ = '\0';
			if(sscanf(history, "%zu", &i) < 1) {
				free(que);
				free(hist);
				free_namelist(paths, paths_len);
				*reason = "Bad Request";
				return 400;
			}
		} else {
			hist = NULL;
			i = namelist_get_arr_ind(paths, paths_len, i);
			if(i == 0) i = paths_len;
			i--;
			i %= paths_len;
			i = paths[i].id;
		}
		break;
	case SET:
		if(body[3] != '=' || sscanf(body + 4, "%zu", &i) < 1) {
			free(hist);
			free_namelist(paths, paths_len);
			*reason = "Bad Request";
			return 400;
		}
		break;
	}
	if(!update_header(&resp_hdrs, strdup("Vary"), strdup("Cookie"))) {
		free(que);
		free(hist);
		free_namelist(paths, paths_len);
		return 500;
	}
	char *cookie_str;
	if(cmd != QUEUE && cmd != UNQUEUE) {
		if(logfile >= 0) dprintf(logfile, "LOG: i = %zu\n", i);
		if(asprintf(&cookie_str, "index=%zu; Path=/", i) < 0) {
			free(que);
			free(hist);
			free_namelist(paths, paths_len);
			free_headers(resp_hdrs);
			return 500;
		}
		if(!add_header(&resp_hdrs, strdup("Set-Cookie"), cookie_str)) {
			free(que);
			free(hist);
			free(cookie_str);
			free_namelist(paths, paths_len);
			free_headers(resp_hdrs);
			return 500;
		}
	}
	if(cmd == NEXT || (cmd == PREV && hist) || cmd == SET) {
		if(asprintf(&cookie_str, "history=%s; Path=/", hist) < 0) {
			free(que);
			free(hist);
			free_namelist(paths, paths_len);
			free_headers(resp_hdrs);
			return 500;
		}
		if(!add_header(&resp_hdrs, strdup("Set-Cookie"), cookie_str)) {
			free(que);
			free(hist);
			free(cookie_str);
			free_namelist(paths, paths_len);
			free_headers(resp_hdrs);
			return 500;
		}
	}
	if((cmd == NEXT && que) || cmd == QUEUE || cmd == UNQUEUE) {
		if(asprintf(&cookie_str, "queue=%s; Path=/", que) < 0) {
			free(que);
			free(hist);
			free_namelist(paths, paths_len);
			free_headers(resp_hdrs);
			return 500;
		}
		if(!add_header(&resp_hdrs, strdup("Set-Cookie"), cookie_str)) {
			free(que);
			free(hist);
			free(cookie_str);
			free_namelist(paths, paths_len);
			free_headers(resp_hdrs);
			return 500;
		}
	}
	char *output;
	size_t len;
	char *hout = NULL;
	int hlen;
	if(hist) {
		hout = strdup("[");
		if(!hout) {
			free(que);
			free(hist);
			free_namelist(paths, paths_len);
			free_headers(resp_hdrs);
			return 500;
		}
		size_t k;
		for(char *c = strrchr(hist, ':'); c; c = strrchr(hist, ':')) {
			*c++ = '\0';
			if(sscanf(c, "%zu", &k) == 1) {
				char *tmp, *namebuf, *name;
				namebuf = strdup(namelist_get_name_by_id(paths, paths_len, k));
				if(!namebuf) {
					free(hout);
					free(que);
					free(hist);
					free_namelist(paths, paths_len);
					free_headers(resp_hdrs);
					return 500;
				}
				name = strrchr(namebuf, '/');
				name = (name) ? name + 1 : namebuf;
				char *dot = strrchr(name, '.');
				if(dot) *dot = '\0';
				if(asprintf(&tmp, "%s\"%s\",", hout, name) < 0) {
					free(namebuf);
					free(hout);
					free(que);
					free(hist);
					free_namelist(paths, paths_len);
					free_headers(resp_hdrs);
					return 500;
				}
				free(namebuf);
				free(hout);
				hout = tmp;
			}
		}
		hlen = strlen(hout);
		hout[(hlen > 1) ? hlen - 1 : hlen++] = ']';
		free(hist);
	}
	char *qout = NULL;
	int qlen;
	if(que) {
		qout = strdup("[");
		if(!qout) {
			free(hout);
			free(que);
			free_namelist(paths, paths_len);
			free_headers(resp_hdrs);
			return 500;
		}
		size_t k;
		for(char *c = strrchr(que, ':'); c; c = strrchr(que, ':')) {
			*c++ = '\0';
			if(sscanf(c, "%zu", &k) == 1) {
				char *tmp, *namebuf, *name;
				namebuf = strdup(namelist_get_name_by_id(paths, paths_len, k));
				if(!namebuf) {
					free(qout);
					free(hout);
					free(que);
					free_namelist(paths, paths_len);
					free_headers(resp_hdrs);
					return 500;
				}
				name = strrchr(namebuf, '/');
				name = (name) ? name + 1 : namebuf;
				char *dot = strrchr(name, '.');
				if(dot) *dot = '\0';
				if(asprintf(&tmp, "%s\"%s\",", qout, name) < 0) {
					free(namebuf);
					free(qout);
					free(hout);
					free(que);
					free_namelist(paths, paths_len);
					free_headers(resp_hdrs);
					return 500;
				}
				free(namebuf);
				free(qout);
				qout = tmp;
			}
		}
		qlen = strlen(qout);
		qout[(qlen > 1) ? qlen - 1 : qlen++] = ']';
		free(que);
	}
	char *buf = strdup(namelist_get_name_by_id(paths, paths_len, i));
	if(!buf) {
		free(qout);
		free(hout);
		free_namelist(paths, paths_len);
		free_headers(resp_hdrs);
		return 500;
	}
	char *name = strrchr(buf, '/');
	name = (name) ? name + 1 : buf;
	char *dot = strrchr(name, '.');
	if(dot) *dot = '\0';
	if(asprintf(&output, "{\"id\": %zu, \"name\": \"%s\"%s%.*s%s%.*s}", i, name, (qout) ? ", \"queue\": " : "", (qout) ? qlen : 0, (qout) ? qout : "", (hout) ? ", \"history\": " : "", (hout) ? hlen : 0, (hout) ? hout : "") < 0) {
		free(buf);
		free(qout);
		free(hout);
		free_namelist(paths, paths_len);
		free_headers(resp_hdrs);
		return 500;
	}
	free(buf);
	free(qout);
	free(hout);
	len = strlen(output);
	int pipefd[2];
	if(pipe(pipefd) < 0) {
		free(output);
		free_namelist(paths, paths_len);
		free_headers(resp_hdrs);
		return 500;
	}
	ssize_t rv;
	while((rv = write(pipefd[1], output, len)) < len) {
		if(rv < 0) {
			switch(errno) {
			case EAGAIN:
			case EINTR:
				continue;
			}
			free(output);
			close(pipefd[0]);
			close(pipefd[1]);
			free_namelist(paths, paths_len);
			free_headers(resp_hdrs);
			return 500;
		}
	}
	free(output);
	close(pipefd[1]);
	free_namelist(paths, paths_len);
	*content_type = strdup("text/plain; charset=utf-8");
	if(!*content_type) {
		close(pipefd[0]);
		free_headers(resp_hdrs);
		return 500;
	}
	*etag = NULL; // not yet implemented
	*content_len = len;
	*ctime = t;
	*h = resp_hdrs;
	*out_fd = pipefd[0];
	*reason = "ok";
	return 200;
}

#ifndef CACHE_FILE
#define CACHE_FILE "paths.cache"
#endif

#ifndef LOGFILE
#define LOGFILE "http_music_player.log"
#endif

#ifndef PIDFILE
#define PIDFILE "/run/http_music_player.pid"
#endif

int main(int argc, char **argv) {
	// parse args
	static struct option long_opts[] = {
		{"html-file",	required_argument,	NULL,	'H'},
		{"cache-file",	required_argument,	NULL,	'C'},
		{"log-file",	required_argument,	NULL,	'L'},
		{"verbose",	optional_argument,	NULL,	'v'},
		{"quiet",	no_argument,		NULL,	'q'},
		{"port",	required_argument,	NULL,	'p'},
		{"daemonize",	no_argument,		NULL,	'D'},
		{"pid-file",	required_argument,	NULL,	'P'},
		{NULL,		0,			NULL,	0}
	};
	int opt;
	char *cache_file = CACHE_FILE;
	char *logfilename = LOGFILE;
	char *pidfile = PIDFILE;
	int port = -1;
	bool daemonize = false;
	while((opt = getopt_long(argc, argv, "H:C:L:v::p:DP:q", long_opts, NULL)) != -1) {
		switch(opt) {
		case 'H':
			html_file = optarg;
			break;
		case 'C':
			cache_file = optarg;
			break;
		case 'L':
		case 'v':
			logfilename = (optarg) ? optarg : LOGFILE;
			break;
		case 'q':
			logfilename = NULL;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'D':
			daemonize = true;
			break;
		case 'P':
			pidfile = optarg;
			break;
		default:
			return 1;
		}
	}
	if(logfilename && ((logfile = open(logfilename, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) < 0)) {
		return 1;
	}
	if(daemonize) {
		// fork once
		switch(fork()) {
		case -1:
			// error
			if(logfile >= 0) {
				dprintf(logfile, "fork: %s", strerror(errno));
				close(logfile);
			}
			return 1;
		case 0:
			// child
			break;
		default:
			// exit in the parent
			if(logfile >= 0) close(logfile);
			return 0;
		}
		// setsid
		if(setsid() < 0) {
			if(logfile >= 0) {
				dprintf(logfile, "setsid: %s", strerror(errno));
				close(logfile);
			}
			return 1;
		}
		// fork again
		pid_t pid = fork();
		if(pid < 0) {
			// error
			if(logfile >= 0) {
				dprintf(logfile, "fork: %s", strerror(errno));
				close(logfile);
			}
			return 1;
		}
		if(pid) {
			// parent
			// create pid file
			int pidfd = open(pidfile, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
			if(pidfd < 0) {
				if(logfile >= 0) {
					dprintf(logfile, "open(%s): %s", pidfile, strerror(errno));
					close(logfile);
				}
				kill(pid, SIGTERM); // kill child if we failed to create the pid file
				return 1;
			}
			// write pid of child, which is the daemon, to the pid file
			if(dprintf(pidfd, "%jd\n", (intmax_t)pid) < 0) {
				if(logfile >= 0) {
					dprintf(logfile, "Error writing pid to pidfile: %s", strerror(errno));
					close(logfile);
				}
				kill(pid, SIGTERM); // kill child if we failed to write the pid to the pid file
				return 1;
			}
			// close the pid file
			if(close(pidfd) < 0) {
				if(logfile >= 0) {
					dprintf(logfile, "close: %s", strerror(errno));
					close(logfile);
				}
				return 1;
			}
			if(logfile >= 0) {
				dprintf(logfile, "LOG: parent pid: %jd\n", (intmax_t)pid);
				close(logfile);
			}
			return 0;
		}
		// change directory to root so we are not using any other directory
		if(chdir("/") < 0) {
			if(logfile >= 0) {
				dprintf(logfile, "chdir: %s", strerror(errno));
				close(logfile);
			}
			return 1;
		}
		umask(0);
		// close stdin, stdout, and stderr
		close(0);
		close(1);
		close(2);
		// open /dev/null as stdin
		if(open("/dev/null", O_RDONLY) < 0) {
			if(logfile >= 0) {
				dprintf(logfile, "open(\"/dev/null\"): %s", strerror(errno));
				close(logfile);
			}
			return 1;
		}
		if(logfile >= 0) {
			// use logfile as stdout and stderr
			if(dup(logfile) < 0) {
				dprintf(logfile, "dup(logfile): %s", strerror(errno));
				close(logfile);
				return 1;
			}
			if(dup(logfile) < 0) {
				dprintf(logfile, "dup(logfile): %s", strerror(errno));
				close(logfile);
				return 1;
			}
		} else {
			// open /dev/null as stdout and stderr
			if(open("/dev/null", O_RDONLY) < 0) {
				if(logfile >= 0) {
					dprintf(logfile, "open(\"/dev/null\"): %s", strerror(errno));
					close(logfile);
				}
				return 1;
			}
			if(open("/dev/null", O_RDONLY) < 0) {
				if(logfile >= 0) {
					dprintf(logfile, "open(\"/dev/null\"): %s", strerror(errno));
					close(logfile);
				}
				return 1;
			}
		}
	}
	page_size = sysconf(_SC_PAGESIZE);
	if(page_size < 0) {
		if(logfile >= 0) {
			dprintf(logfile, "sysconf: %s", strerror(errno));
			close(logfile);
		}
		return 1;
	}
	shvars = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if(shvars == MAP_FAILED) {
		if(logfile >= 0) {
			dprintf(logfile, "mmap: %s", strerror(errno));
			close(logfile);
		}
		return 1;
	}
	shvars->shm_fd = shm_open("/shmem", O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	if(shvars->shm_fd < 0) {
		if(logfile >= 0) {
			dprintf(logfile, "shm_open: %s", strerror(errno));
			close(logfile);
		}
		munmap(shvars, page_size);
		return 1;
	}
	if(access(cache_file, R_OK) == 0) {
		struct stat s = {0};
		if(stat(cache_file, &s) < 0) {
			if(logfile >= 0) {
				dprintf(logfile, "stat: %s", strerror(errno));
				close(logfile);
			}
			munmap(shvars, page_size);
			shm_unlink("/shmem");
			return 1;
		}
		if(ftruncate(shvars->shm_fd, s.st_size) < 0) {
			if(logfile >= 0) {
				dprintf(logfile, "ftruncate: %s", strerror(errno));
				close(logfile);
			}
			munmap(shvars, page_size);
			shm_unlink("/shmem");
			return 1;
		}
		shvars->shmem_size = s.st_size;
		int fd = open(cache_file, O_RDONLY);
		if(fd < 0) {
			if(logfile >= 0) {
				dprintf(logfile, "open: %s", strerror(errno));
				close(logfile);
			}
			munmap(shvars, page_size);
			shm_unlink("/shmem");
			return 1;
		}
		void *contents = mmap(NULL, s.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		int err = errno;
		close(fd);
		errno = err;
		if(contents == MAP_FAILED) {
			if(logfile >= 0) {
				dprintf(logfile, "mmap: %s", strerror(errno));
				close(logfile);
			}
			munmap(shvars, page_size);
			shm_unlink("/shmem");
			return 1;
		}
		shvars->shmem = mmap(NULL, shvars->shmem_size, PROT_WRITE, MAP_SHARED, shvars->shm_fd, 0);
		if(contents == MAP_FAILED) {
			if(logfile >= 0) {
				dprintf(logfile, "mmap: %s", strerror(errno));
				close(logfile);
			}
			munmap(shvars, page_size);
			shm_unlink("/shmem");
			munmap(contents, s.st_size);
			return 1;
		}
		memcpy(shvars->shmem, contents, s.st_size);
		shvars->shmem_used = s.st_size;
		munmap(contents, s.st_size);
		munmap(shvars->shmem, s.st_size);
	} else {
		if(ftruncate(shvars->shm_fd, page_size) < 0) {
			if(logfile >= 0) {
				dprintf(logfile, "ftruncate: %s", strerror(errno));
				close(logfile);
			}
			munmap(shvars, page_size);
			shm_unlink("/shmem");
			return 1;
		}
		shvars->shmem_size = page_size;
		shvars->shmem_used = 0;
	}
	pthread_rwlockattr_t attr;
	if(pthread_rwlockattr_init(&attr)) {
		munmap(shvars, page_size);
		shm_unlink("/shmem");
		if(logfile >= 0) close(logfile);
		return 1;
	}
	if(pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED)) {
		munmap(shvars, page_size);
		pthread_rwlockattr_destroy(&attr);
		shm_unlink("/shmem");
		if(logfile >= 0) close(logfile);
		return 1;
	}
	if(pthread_rwlock_init(&shvars->rwlock, &attr)) {
		munmap(shvars, page_size);
		pthread_rwlockattr_destroy(&attr);
		shm_unlink("/shmem");
		if(logfile >= 0) close(logfile);
		return 1;
	}
	pthread_rwlockattr_destroy(&attr);
	struct HTTP_Request_Handlers hls = {0};
	hls.get_req_handler = get_response;
	hls.post_req_handler = post_response;
	int retval = !server((argc > optind) ? argv[optind] : NULL, hls, logfilename, port);
	pthread_rwlock_destroy(&shvars->rwlock);
	if(!child && shvars->shmem_used) {
		int fd = open(cache_file, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
		if(fd < 0) {
			if(logfile >= 0) {
				dprintf(logfile, "open: %s", strerror(errno));
				close(logfile);
			}
			munmap(shvars, page_size);
			shm_unlink("/shmem");
			return 1;
		}
		if(ftruncate(fd, shvars->shmem_used) < 0) {
			if(logfile >= 0) {
				dprintf(logfile, "ftruncate: %s", strerror(errno));
				close(logfile);
			}
			munmap(shvars, page_size);
			shm_unlink("/shmem");
			close(fd);
			return 1;
		}
		void *contents = mmap(NULL, shvars->shmem_used, PROT_WRITE, MAP_SHARED, fd, 0);
		int err = errno;
		close(fd);
		errno = err;
		if(contents == MAP_FAILED) {
			if(logfile >= 0) {
				dprintf(logfile, "mmap contents: %s", strerror(errno));
				close(logfile);
			}
			munmap(shvars, page_size);
			shm_unlink("/shmem");
			return 1;
		}
		shvars->shmem = mmap(NULL, shvars->shmem_used, PROT_READ, MAP_SHARED, shvars->shm_fd, 0);
		if(shvars->shmem == MAP_FAILED) {
			if(logfile >= 0) {
				dprintf(logfile, "mmap shmem: %s", strerror(errno));
				close(logfile);
			}
			munmap(shvars, page_size);
			munmap(contents, shvars->shmem_size);
			shm_unlink("/shmem");
			return 1;
		}
		memcpy(contents, shvars->shmem, shvars->shmem_used);
		munmap(shvars->shmem, shvars->shmem_used);
		munmap(contents, shvars->shmem_used);
	}
	munmap(shvars, page_size);
	shm_unlink("/shmem");
	if(logfile >= 0) close(logfile);
	return retval;
}
