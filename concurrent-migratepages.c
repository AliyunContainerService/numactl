#define _GNU_SOURCE
#include <getopt.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <numa.h>
#include <numaif.h>
#include "threadpool.h"
//#include "tpool.h"

#define MAX_PAGES 8192
#define BUF_SIZE  1024
//#define WORKER_THREAD_NUMBER 1024
#define MAX_WORKER_THREAD 64
#define THREAD_GEN_PER_TIME 64
#define MAX_TASK_SEQUEEZE 1024
#define RETRY_TIMES 5

static int move_cnts;
static long total_pages;

struct option opts[] = {
	{"help", 0, 0, 'h' },
	{ 0 }
};

struct worker_arg {
    int pid;
    int page_count;
    void *addr[MAX_PAGES];
    int nodes[MAX_PAGES];
};

void usage(void)
{
	fprintf(stderr,
		"usage: migratepages-demo pid to-node\n"
        "in this version it is only allowed to migrate to only 1 node"
		"\n"
);
	exit(1);
}

void checknuma(void)
{
	static int numa = -1;
	if (numa < 0) {
		if (numa_available() < 0)
			perror("This system does not support NUMA functionality");
	}
	numa = 0;
}

void *do_work(void *arg)
{
    move_cnts++;
    struct worker_arg *warg = (struct worker_arg *)arg;
    total_pages += warg->page_count;
    int status[MAX_PAGES];
    int rc, i;
    int cnt = 0;
    
    //for( i = 0; i < warg->page_count; i++) 
    //    printf("addr[i]: %lx\n", warg->addr[i]);
    rc = move_pages(warg->pid, warg->page_count, warg->addr, warg->nodes, status,MPOL_MF_MOVE_ALL);
    if (rc < 0 && errno != ENOENT) {
        printf("Failed to migrate %d pages from %lx\n ", warg->page_count, warg->addr[0]);
    }
    //printf("afteraddr[0]: %lx\n", warg->addr[0]);
    free(arg);
    return NULL;
}

int main(int argc, char *argv[])
{   
	int c;
	char *end;
	int pid;
    int tonode;
    int i, j;

    unsigned int pagesize;

    int nr_nodes;
    char maps_file[BUF_SIZE] = {0};
    char numa_maps_file[BUF_SIZE] = {0};
    char maps_buf[BUF_SIZE];
    char numa_maps_buf[BUF_SIZE];
    int nr_addrs = 0;
    long real_total_pages = 0;
    total_pages = 0;
    void *addr[MAX_PAGES];
    int nodes[MAX_PAGES];
    

    //clock_t begin, before_create_threadpool, till_created_threadpool, till_ended_migration, till_destoryed_threadpool;
    //double init_time, create_threadpool_time, migrate_and_IO_time, destory_threadpool_time;
    
    move_cnts = 0;
    //printf("Begin to initialize.\n");
    //begin = clock();

	while ((c = getopt_long(argc,argv,"h", opts, NULL)) != -1) {
		switch (c) {
		default:
			usage();
		}
	}

	argv += optind;
	argc -= optind;

	if (argc != 2)
		usage();

	checknuma();

	pid = strtoul(argv[0], &end, 0);
	if (*end || end == argv[0])
		usage();

    tonode = strtoul(argv[1], &end, 0);
    if (*end || end == argv[1])
        usage();

    //rc = numa_migrate_pages(pid, fromnodes, tonodes);
    pagesize = getpagesize();
    nr_nodes = numa_max_node()+1;

    if (tonode >= nr_nodes)
    {
        perror("invalid node ID.");
		return 1;
    }

    for (j = 0; j< MAX_PAGES; j++)
		nodes[j] = tonode;

    sprintf(maps_file, "/proc/%d/maps",pid);
    FILE *maps_info = fopen(maps_file, "r");
    if(!maps_info) {
        printf("Tried to research PID %d maps, but it apparently went away.\n", pid);
        return 0;
    }

    sprintf(numa_maps_file, "/proc/%d/numa_maps",pid);
    FILE *numa_maps_info = fopen(numa_maps_file, "r");
    if(!numa_maps_info) {
        printf("Tried to research PID %d numa_maps, but it apparently went away.\n", pid);
        return 0;
    }


    //printf("Begin to create the threadpool.\n");
    //before_create_threadpool = clock();
    /* 线程池初始化，其管理者线程及工作线程都会启动 */
    //threadpool_t *thp = threadpool_create(10, 100, 1024);

    tpool_t* pool = NULL;
    if(0 != create_tpool(&pool,MAX_WORKER_THREAD)){
        perror("create_tpool failed!\n");
        return -1;
    }

    //tThreadpoolInfo *threadpool;
    //threadpool = threadpool_create(MAX_WORKER_THREAD, THREAD_GEN_PER_TIME, MAX_TASK_SEQUEEZE);
    
    //printf("Begin to migrate the memory in use.\n");
    //till_created_threadpool = clock();

    while (fgets(numa_maps_buf, BUF_SIZE, numa_maps_info) && fgets(maps_buf, BUF_SIZE, maps_info)) {
        // maps在结尾处比numa_maps多一行内核空间的信息不需要遍历
        // 先遍历numa_maps_info查看是否有非target node上的页
        const char *numa_maps_delimiters = " ";
        char *numa_maps_startpos = strtok(numa_maps_buf, numa_maps_delimiters);
        char *str_p = strtok(NULL, numa_maps_delimiters);
        while (str_p) {
            if (*str_p++ == 'N') {
                int node;
                node = *str_p++ - '0';
                if (*str_p++ != '=') {
                    printf("numa_maps node number parse error\n");
                    printf("contain: %s\n", str_p);
                    exit(1);
                }
                if (node != tonode) {
                    // 存在需要搬移的页面，解析maps
                    const char *maps_delimiters = "-";
                    char *startpos = strtok(maps_buf, maps_delimiters);  
                    if (strcmp(numa_maps_startpos, startpos) != 0) {
                        printf("numa_maps doesnt match maps.\n");
                        exit(1);
                    }      
                    char *endpos = strtok(NULL, maps_delimiters);
                    if (strlen(startpos) > 12 || 
                        (strlen(startpos) == 12 && 
                            (startpos[0] > '7' || startpos[0] < '0'))) {
                                // 不属于user space不搬移
                                continue;
                            }
                    char *tmp;
                    long start16 = strtol(startpos, &tmp, 16);
                    long end16 = strtol(endpos, &tmp, 16);
                    long cnt = (end16 - start16) / pagesize;
                    real_total_pages += cnt;
                    //printf("begin address: %ld\n", start16);
                    for (i = 0; i < cnt; ++i) {
                        addr[nr_addrs] = (void *)(start16) + i * pagesize;
                        //nodes[nr_addrs] = tonode; 
                        if (nr_addrs == MAX_PAGES - 1) {
                            // 分发任务给工作线程
                            /* 接收到任务后添加 */
                            //threadpool_add_task(thp, do_work, (void *)&arg);
                            struct worker_arg *arg;
                            arg = malloc(sizeof(struct worker_arg));
                            arg->page_count = MAX_PAGES;
                            arg->pid = pid;
                            memcpy(arg->nodes, nodes, sizeof(nodes));
                            memcpy(arg->addr, addr, sizeof(addr));
                            add_task_2_tpool(pool, do_work ,(void*)arg);
                            //if (0 != threadpool_addtask(threadpool, (THREADPOOLTASKFUNC)do_work, (void *)&arg[nr_args++], NULL))
                            //{
                            //    printf("Faile to add task to threadpool.\n");
                            //}
                            nr_addrs = 0;
                            memset(addr,0,sizeof(addr));
                        } else {
                            nr_addrs++;
                        }
                    }
                    break;
                }
            } 
            str_p = strtok(NULL, numa_maps_delimiters);
        }
    }
    
    if (nr_addrs != 0) {
        // 剩余的也打包成一个任务
        //printf("To deal with the rest part with length %ld\n", nr_addrs);
        //struct worker_arg_rest
        //{
        //    int pid;
        //    int page_count;
        //    void *addr[nr_addrs];
        //    int nodes[nr_addrs];
        //};
        
        struct worker_arg *arg;
        arg = malloc(sizeof(struct worker_arg));
        arg->page_count = nr_addrs;
        arg->pid = pid;
        memcpy(arg->nodes, nodes, sizeof(nodes));
        memcpy(arg->addr, addr, sizeof(addr));
        //threadpool_add_task(thp, do_work, (void *)&arg);
        add_task_2_tpool(pool, do_work ,(void*)arg);
        //if (0 != threadpool_addtask(threadpool, (THREADPOOLTASKFUNC)do_work, (void *)&arg[nr_args], NULL))
        //{
        //    printf("Faile to add task to threadpool.\n");
        //}
    }

    //printf("Begin to destory the threadpool.\n");
    //till_ended_migration = clock();

    /* 销毁 */
    //threadpool_destroy(thp);
    destroy_tpool(pool);
    //threadpool_destroy(threadpool);

    //till_destoryed_threadpool = clock();

    //init_time = (double)(before_create_threadpool - begin)/CLOCKS_PER_SEC;
    //create_threadpool_time = (double)(till_created_threadpool - before_create_threadpool)/CLOCKS_PER_SEC;
    //migrate_and_IO_time = (double)(till_ended_migration - till_created_threadpool)/CLOCKS_PER_SEC;
    //destory_threadpool_time = (double)(till_destoryed_threadpool - till_ended_migration)/CLOCKS_PER_SEC;
    //printf("Inital time: %lf secs \n", init_time);
    //printf("Create threadpool time: %lf secs \n", create_threadpool_time);
    //printf("Migrate and IO time: %lf secs \n", migrate_and_IO_time);
    //printf("Destory threadpool time: %lf secs \n", destory_threadpool_time);
    printf("Totally migrate %ld pages \n", total_pages);
    printf("Should be migrated %ld.\n", real_total_pages);
    printf("Totally migrate %d times \n", move_cnts);
    
    fclose(maps_info);
	return 0;
}
