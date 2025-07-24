#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <time.h>
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>


#define MAX_PROCS 1024
#define MAX_LOGO_LINES 128
#define MAX_LINE_LEN 256

typedef struct {
    int pid;
    unsigned long long prev_utime;
} proc_state_t;

typedef struct {
    int pid;
    char user[32];
    char cmd[64];
    float cpu;
    float mem;
    int tty; // If has tty > treat as interactive
} proc_t;

void draw_bar(float p, int w) {
    int b = (int)(p/100.0f * w);
    printf("[");
    for (int i = 0; i < w; i++) printf(i<b ? "#" : "-");
    printf("] %5.1f%%", p);
}

void get_per_core_usage(unsigned long long *total, unsigned long long *idle, int cores) {
    FILE *fp = fopen("/proc/stat", "r"); if (!fp) return;
    char line[512]; fgets(line,sizeof(line),fp);
    for (int i=0;i<cores;i++) {
        fgets(line,sizeof(line),fp);
        char cpu[10]; unsigned long long u,n,s,idleval,iowait,irq,sirq,steal,g,gn;
        sscanf(line,"%s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
            cpu,&u,&n,&s,&idleval,&iowait,&irq,&sirq,&steal,&g,&gn);
        total[i] = u+n+s+idleval+iowait+irq+sirq+steal+g+gn;
        idle[i] = idleval+iowait;
    }
    fclose(fp);
}

float get_ram_usage() {
    FILE *fp = fopen("/proc/meminfo","r"); if (!fp) return -1;
    char l[256]; unsigned long t=0,f=0,b=0,c=0;
    while (fgets(l,sizeof(l),fp)) {
        sscanf(l,"MemTotal: %lu",&t);
        sscanf(l,"MemFree: %lu",&f);
        sscanf(l,"Buffers: %lu",&b);
        sscanf(l,"Cached: %lu",&c);
    }
    fclose(fp);
    return (float)(t-f-b-c)/t*100.0f;
}

float get_disk_usage() {
    struct statvfs v; if(statvfs("/",&v)) return -1;
    unsigned long t = v.f_blocks * v.f_frsize;
    unsigned long f = v.f_bfree * v.f_frsize;
    return (float)(t-f)/t*100.0f;
}

float get_battery() {
    FILE *fp = fopen("/sys/class/power_supply/BAT0/capacity","r");
    if (!fp) return -1; int cap; fscanf(fp,"%d",&cap); fclose(fp);
    return cap;
}

void get_os_pretty(char *d,size_t l) {
    FILE *fp = fopen("/etc/os-release","r");
    if (!fp) { snprintf(d,l,"Unknown"); return; }
    char line[256];
    while (fgets(line,sizeof(line),fp)) {
        if (strncmp(line,"PRETTY_NAME=",12)==0) {
            char *s=strchr(line,'"'), *e=strrchr(line,'"');
            if (s && e && e>s) {
                size_t m = e-s-1; if (m>=l) m = l-1;
                strncpy(d,s+1,m); d[m]=0; fclose(fp); return;
            }
        }
    } fclose(fp); snprintf(d,l,"Unknown");
}

void get_cpu_model(char *d,size_t l) {
    FILE *fp = fopen("/proc/cpuinfo","r");
    if (!fp) { snprintf(d,l,"Unknown"); return; }
    char line[256];
    while (fgets(line,sizeof(line),fp)) {
        if (strncmp(line,"model name",10)==0) {
            char *c=strchr(line,':'); if (c) while (*++c==' ');
            strncpy(d,c,l-1); d[l-1]=0; char *n=strchr(d,'\n');
            if (n) *n=0; fclose(fp); return;
        }
    } fclose(fp); snprintf(d,l,"Unknown");
}

unsigned long long get_total_jiffies() {
    FILE *fp = fopen("/proc/stat","r"); if (!fp) return 0;
    char line[512]; fgets(line,sizeof(line),fp); fclose(fp);
    unsigned long long u,n,s,iowait,irq,softirq,steal,idle;
    sscanf(line,"cpu %llu %llu %llu %llu %llu %llu %llu %llu",
        &u,&n,&s,&idle,&iowait,&irq,&softirq,&steal);
    return u+n+s+idle+iowait+irq+softirq+steal;
}

unsigned long long find_prev(proc_state_t *states, int n, int pid) {
    for (int i = 0; i < n; i++) if (states[i].pid == pid) return states[i].prev_utime;
    return 0;
}
void update_state(proc_state_t *states, int *n, int pid, unsigned long long utime) {
    for (int i = 0; i < *n; i++) if (states[i].pid == pid) { states[i].prev_utime = utime; return; }
    if (*n < MAX_PROCS) { states[*n].pid = pid; states[*n].prev_utime = utime; (*n)++; }
}

int cmp_mem(const void *a,const void *b) {
    float diff = ((proc_t*)b)->mem - ((proc_t*)a)->mem;
    return diff < 0 ? -1 : (diff > 0 ? 1 : 0);
}
int cmp_cpu(const void *a,const void *b) {
    float diff = ((proc_t*)b)->cpu - ((proc_t*)a)->cpu;
    return diff < 0 ? -1 : (diff > 0 ? 1 : 0);
}
int cmp_pid(const void *a,const void *b) {
    return ((proc_t*)a)->pid - ((proc_t*)b)->pid;
}
int cmp_cmd(const void *a,const void *b) {
    return strcmp(((proc_t*)a)->cmd, ((proc_t*)b)->cmd);
}

void get_top(proc_t *procs,int *count,int max,unsigned long long sys_prev,unsigned long long sys_now,
             proc_state_t *states,int *state_count,int cores) {
    DIR *dir = opendir("/proc"); if (!dir) return;
    struct dirent *e; int found = 0;
    while ((e = readdir(dir))) {
        if (e->d_type != DT_DIR) continue;
        int pid = atoi(e->d_name); if (pid <= 0) continue;

        char s[128]; snprintf(s,sizeof(s),"/proc/%d/stat",pid);
        FILE *fp = fopen(s,"r"); if (!fp) continue;
        char comm[64]; char state;
        unsigned long long ut, st;
        fscanf(fp,"%*d %s %c",&comm,&state);
        for (int i = 0; i < 11; i++) fscanf(fp,"%*s");
        fscanf(fp,"%llu %llu",&ut,&st);
        int tty_nr; fscanf(fp,"%*s %*s %*s %*s %*s %*s %*s %*s %*s %d", &tty_nr);
        fclose(fp);

        // Filter: if no tty, treat as daemon/system
        if (tty_nr == 0) continue;

        struct stat stbuf; char p[128];
        snprintf(p,sizeof(p),"/proc/%d",pid); stat(p,&stbuf);
        struct passwd *pw = getpwuid(stbuf.st_uid);

        snprintf(procs[found].user,31,"%s",pw?pw->pw_name:"?");
        snprintf(procs[found].cmd,63,"%s",comm);
        procs[found].pid = pid;
        procs[found].tty = tty_nr;

        unsigned long long last = find_prev(states, *state_count, pid);
        update_state(states, state_count, pid, ut+st);
        procs[found].cpu = (sys_now != sys_prev) ? (float)(ut+st - last) / (sys_now - sys_prev) * 100.0f * cores : 0;

        snprintf(s,sizeof(s),"/proc/%d/statm",pid);
        fp = fopen(s,"r"); if (!fp) continue;
        unsigned long res; fscanf(fp,"%*s %lu",&res); fclose(fp);
        procs[found].mem = (float)res * getpagesize() / 1024 / 1024;

        found++;
        if (found >= max) break;
    }
    closedir(dir);
    *count = found;
}

// --- raw term ---
void set_raw(struct termios *old) {
    struct termios raw;
    tcgetattr(0, old);
    raw = *old;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &raw);
}
void restore_raw(struct termios *old) { tcsetattr(0, TCSANOW, old); }

int main() {
    struct utsname sys; uname(&sys);
    int cores = sysconf(_SC_NPROCESSORS_ONLN);
    unsigned long long prev_total[cores], prev_idle[cores];
    get_per_core_usage(prev_total, prev_idle, cores);
    unsigned long long sys_prev = get_total_jiffies();

    char pretty[128], cpu[128]; get_os_pretty(pretty,128); get_cpu_model(cpu,128);
    char *u = getenv("USER"); char host[256]; gethostname(host,256);

    float cached_bat = -1; time_t last_bat = 0;
    proc_state_t states[MAX_PROCS]; int state_count = 0;

    char *logo[MAX_LOGO_LINES]; int logo_lines = 0;
    FILE *fp = fopen("config.txt","r");
    if (fp) { char line[MAX_LINE_LEN];
        while (fgets(line,sizeof(line),fp) && logo_lines < MAX_LOGO_LINES) {
            line[strcspn(line,"\n")] = 0; logo[logo_lines] = strdup(line); logo_lines++;
        } fclose(fp);
    }

    struct termios old;
    set_raw(&old);
    int offset = 0;
    int sort_mode = 1;

    while (1) {
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    if (ws.ws_col < 116 || ws.ws_row < 47) {
        printf("\033[H\033[2J");
        printf("\n[!] Terminal window too small!\n");
        printf("    Minimum: 116 cols x 47 rows\n");
        printf("    Current: %d cols x %d rows\n", ws.ws_col, ws.ws_row);
        printf("    Please resize your terminal.\n");
        fflush(stdout);
        usleep(500000);
        continue;
        }
        printf("\033[H\033[2J");
        int lines = cores + 6; int max_lines = lines > logo_lines ? lines : logo_lines;
        for (int i = 0; i < max_lines; i++) {
            if (i < logo_lines) printf("%-40s ", logo[i]); else printf("%-40s ", " ");
            if (i == 0) printf(" : %s @ %s\n", u, host);
            else if (i == 1) printf("󰣇 OS : %s\n", pretty);
            else if (i == 2) printf(" Kernel : %s\n", sys.release);
            else if (i == 3) printf(" CPU : %s\n", cpu);
            else if (i >= 4 && i < 4+cores) {
                unsigned long long total[cores], idle[cores];
                get_per_core_usage(total, idle, cores);
                int core = i-4;
                unsigned long long dt = total[core]-prev_total[core], di = idle[core]-prev_idle[core];
                float u = dt ? (float)(dt-di)/dt*100.0f : 0;
                printf("CPU Core %2d: ", core); draw_bar(u, 20); printf("\n");
                prev_total[core]=total[core]; prev_idle[core]=idle[core];
            } else if (i == 4+cores) {
                printf(" RAM : "); draw_bar(get_ram_usage(), 20); printf("\n");
            } else if (i == 5+cores) {
                printf(" Disk: "); draw_bar(get_disk_usage(), 20); printf("\n");
            } else printf("\n");
        }

        time_t now = time(NULL);
        if (now-last_bat > 5) { cached_bat = get_battery(); last_bat = now; }
        printf("\nBattery: "); draw_bar(cached_bat, 25); printf("\n");

        unsigned long long sys_now = get_total_jiffies();
        proc_t top[MAX_PROCS]; int num=0; get_top(top, &num, MAX_PROCS, sys_prev, sys_now, states, &state_count, cores);
        sys_prev = sys_now;

        if (sort_mode == 1) qsort(top, num, sizeof(proc_t), cmp_cpu);
        else if (sort_mode == 2) qsort(top, num, sizeof(proc_t), cmp_mem);
        else if (sort_mode == 3) qsort(top, num, sizeof(proc_t), cmp_pid);
        else qsort(top, num, sizeof(proc_t), cmp_cmd);

        printf("\n%-6s %-8s %-6s %-6s %-s\n","PID","USER","CPU%","MEM(MB)","CMD");
        int show = 11;
        if (offset > num - show) offset = num - show; if (offset < 0) offset = 0;
        for (int i = offset; i < offset + show && i < num; i++)
            printf("%-6d %-8s %-6.1f %-8.1f %s\n",
                top[i].pid, top[i].user, top[i].cpu, top[i].mem, top[i].cmd);

        printf("\n[F1:CPU] [F2:MEM] [F3:PID] [F4:CMD]  [↑↓:Scroll]\n");

        fflush(stdout);

        usleep(100000);

        if (read(0, &(char){0}, 1) == 1) {
            char c; read(0,&c,1);
            if (c == '[') {
                read(0,&c,1);
                if (c == 'A') offset--;
                else if (c == 'B') offset++;
            } else if (c == 'O') { // Function keys: ESC O P = F1
                read(0,&c,1);
                if (c == 'P') sort_mode = 1;
                else if (c == 'Q') sort_mode = 2;
                else if (c == 'R') sort_mode = 3;
                else if (c == 'S') sort_mode = 4;
            }
        }
    }

    restore_raw(&old);
    return 0;
}
