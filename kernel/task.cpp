/*
 * Copyright (C) 2011 Taner Guven <tanerguven@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Task (process) olusturma, ilk process'in olusturulmasi, sanal bellek alani
 * olusturma, kernele gomulmus binary programi processe yukleme gibi islemler
 */

#include <types.h>
#include <asm/x86.h>

#include "task.h"
#include "elf.h"

#include "sched.h"
#include "time.h"

#include <string.h>
#include <stdio.h>
using namespace std;

#include "kernel.h"

// task_exit.cpp
extern void task_kill(Task*);
//

// signal.cpp
extern void free_sigstack();
extern void copy_stack(uint32_t addr);
//

/*
 * TODO: signal almis bir process, signal fonksiyonunda fork yaparsa ?
 * TODO: zombie tasklar icin bir cozum
 */

// kernel_monitor.cpp
void parse_cmd(char *cmd, int *argc, char *argv[10]);
//

void switch_to_task(Task *newtask) __attribute__ ((noinline));
void __task_first_run() __attribute__ ((noreturn));

extern "C" void task_first_run();

/* liste tipleri icin nodlarin offset degerleri */
set_list_offset(struct Task, AlarmList_t, alarm_list_node);
set_list_offset(struct Task, ChildList_t, childlist_node);
set_list_offset(struct Task, TaskList_t, list_node);
set_id_hash_offset(struct Task, TaskIdHashTable_t, id_hash_node, id);

TaskIdHashTable_t task_id_ht;
uint8_t mem_task_id_ht[4096];
int next_task_id = 0;

TaskList_t task_zombie_list;
/**
 * task_curr sadece sistem cagrisi icinde kullanilmali.
 * diger durumlarda NULL olabilir. (runnable task yoksa NULL olur)
 */
Task *task_curr;
Task task0; // bostayken kullanilan task

/** task için sanal bellek oluşturur */
static int task_setup_vm(Task *t, PageDirInfo *parent_pgdir) {
	ASSERT(!(eflags_read() & FL_IF));

	int err;
	uint32_t va;
	Page *p;

	memset(&t->pgdir, 0, sizeof(t->pgdir));

	/* page directory yapisi icin bellek al */
	if ((err = tmp_page_alloc_map(&p, &va, PTE_P | PTE_W)) < 0)
		return err;
	t->pgdir.pgdir_pa = p->addr();
	t->pgdir.pgdir = (PageDirectory*)va2kaddr(va);
	memset(t->pgdir.pgdir, 0, 0x1000);
	t->pgdir.count_kernel++;

	/* page table listesi icin bellek al */
	if ((err = tmp_page_alloc_map(&p, &va, PTE_P | PTE_W)) < 0)
		return err;
	t->pgdir.pgtables = (PageTable**)va2kaddr(va);
	memset(t->pgdir.pgtables, 0, 0x1000);
	t->pgdir.count_kernel++;

	/* Kernel adres uzayini dogrudan task adres uzayina bagla */
	err = t->pgdir.link_pgtables(parent_pgdir, MMAP_KERNEL_BASE, MMAP_KERNEL_STACK_BASE);
	ASSERT(err == 0);

	return 0;
}

static void task_delete_vm(Task *t) {
	ASSERT(!(eflags_read() & FL_IF));

	/* task baska taski silemez, killemek icin signal gondermeli */
	ASSERT(t != task_curr);
	uint32_t va;

	/* free pgdir */
	va = (uint32_t)t->pgdir.pgdir;
	if (va > 0) {
		ASSERT( tmp_page_free(kaddr2va(va)) == 0);
		t->pgdir.count_kernel--;
	}

	/* free pgtables */
	va = (uint32_t)t->pgdir.pgtables;
	if (va > 0) {
		ASSERT( tmp_page_free(kaddr2va(va)) == 0);
		t->pgdir.count_kernel--;
	}

	ASSERT(t->pgdir.count_user == 0);
	ASSERT(t->pgdir.count_kernel == 0);
}

int task_create_kernel_stack(Task *t) {
	int r;
	Page *p;

	r = page_alloc(&p);
	if (r < 0)
		return r;
	r = t->pgdir.page_insert(p, MMAP_KERNEL_STACK_BASE, PTE_P | PTE_W);
	if (r < 0)
		return r;

	return 0;
}

int task_copy_kernel_stack(Task *t) {
	int r;
	Page *p;
	uint32_t va;

	r = tmp_page_alloc_map(&p, &va, PTE_P | PTE_W);
	if (r < 0)
		return r;
	r = t->pgdir.page_insert(p, MMAP_KERNEL_STACK_BASE, PTE_P | PTE_W);
	if (r < 0) {
		ASSERT( tmp_page_free(va) == 0);
		return r;
	}

	memcpy((void*)va2kaddr(va),
		   (void*)va2kaddr(MMAP_KERNEL_STACK_BASE), 0x1000);

	ASSERT( tmp_page_free(va) == 1);
	return 0;
}

void task_free_kernel_stack(Task* t) {
	ASSERT(!(eflags_read() & FL_IF));

	int r;
	uint32_t va;

	VA_t va_kernel_stack(MMAP_KERNEL_STACK_TOP - 0x1000);

	/* free kernel stack */
	/*
	 * paylasilamayan alan, silince refcount=0 olmali
	 * kernel stack yoksa r = errno da olabilir
	 */
	r = t->pgdir.page_remove(va_kernel_stack, 1);
	if (r > -1)
		ASSERT( r == 0);

	/* free kernel stacks pgtable */
	va = (uint32_t)t->pgdir.pgtables[va_kernel_stack.pdx];
	if (va > 0) {
		ASSERT( tmp_page_free(kaddr2va(va)) == 0);
		t->pgdir.count_kernel--;
	}

	free_sigstack();
}

/**
 * kernele gomulmus binary programi process'in adres uzayina yukleyen
 * fonksiyon. (exec cagrisinda yapilan islem)
 */
static int load_icode(Task *t, Trapframe *registers, uint32_t prog_addr, const char *cmd) {
	ASSERT(!(eflags_read() & FL_IF));

	int err;
	uint32_t last_addr = 0;

	/*
	 * program kodlarini processin adres uzayina yukleyebilmek icin, gecici
	 * olarak yeni processin pagedir'ini yukle.
	 */
	uint32_t old_cr3 = cr3_read();
	ASSERT(old_cr3 != t->pgdir.pgdir_pa);
	cr3_load(t->pgdir.pgdir_pa);

	/* programın binarisinin adresi, elf header'ı */
	Elf32_Ehdr *header = (Elf32_Ehdr*)prog_addr;
	/* ilk program header'ı (ph), binary'nin başlangıcı + ph offset */
	Elf32_Phdr *ph = (Elf32_Phdr*)(prog_addr + header->phoff);
	/* ph'larının bitiş noktası */
	Elf32_Phdr *ph_end = ph + header->phnum;

	/*
	 * ph'lar dizi şeklinde saklanıyor. ph değişkenini arttırarrak (adresini
	 * sizeof(Elf32_Phdr) kadar ilerleterek) ph_end'e kadar olan tüm ph'ları
	 * tarıyoruz.
	 */
	for ( ; ph < ph_end ; ph++) {
		if (ph->type == Elf32_Phdr::Type_LOAD) {
			err = t->pgdir.segment_alloc(uaddr2va(ph->vaddr), ph->memsz,
										 PTE_P | PTE_U | PTE_W);
			if (err < 0)
				return err;

			if (roundUp(ph->vaddr + ph->memsz) > last_addr)
				last_addr = roundUp(ph->vaddr + ph->memsz);

			t->pgdir.count_program += roundUp(ph->memsz) / 0x1000;
			/* ph tipi, çalıştırılabilir programsa belleğe kopyalıyoruz */
			memcpy((void*)uaddr2kaddr(ph->vaddr),
				   (void*)(prog_addr + ph->offset), ph->filesz);
			/* programını sonunda kalan alanı sıfırlıyoruz */
			if (ph->memsz > ph->filesz)
				memset((void*)(uaddr2kaddr(ph->vaddr) + ph->filesz),
					   0, ph->memsz - ph->filesz);
		}
	}

	/* heap alani */
	t->pgdir.start_brk = t->pgdir.end_brk = last_addr;

	/* processin program counterina programin baslangic adresini ata */
	registers->eip = header->entry;

	/* process icin stack alani olustur */
	err = t->pgdir.page_alloc_insert(MMAP_USER_STACK_TOP - 0x1000,
									 PTE_P | PTE_U | PTE_W);
	if (err < 0)
		return err;
	t->pgdir.count_stack = 1;

	// programin argc ve argv parameterelerini olustur
	/* komut girisini stack alanina yaz */
	int str_size = strlen(cmd) + 1;
	uint32_t *esp = (uint32_t*)va2kaddr(MMAP_USER_STACK_TOP);
	memcpy((char*)esp-str_size, cmd, str_size);
	esp = (uint32_t*)((char*)esp - str_size);

	/* komutu argv biciminde parcala ve adres donusumlerini yap ve stacke bas */
	int argc;
	char **argv;
	parse_cmd((char*)esp, &argc, argv);
	for (int i = 0 ; i < argc ; i++)
		esp[-argc+i] = kaddr2uaddr((uint32_t)argv[i]);
	esp -= argc;

	/* argv, argc, eip degerlerini stacke bas */
	esp[-1] = kaddr2uaddr((uint32_t)esp); /* argv */
	esp[-2] = argc; /* argc */
	esp[-3] = va2uaddr(0xffffffff); /* return eip */
	esp -= 3;

	registers->esp = kaddr2uaddr((uint32_t)esp);
	//

	cr3_load(old_cr3);
	return 0;
}

static void task_free_vm_user(Task* t) {
	ASSERT(!(eflags_read() & FL_IF));

	/*  user alanindaki butun pageleri serbest birak */
	for (uint32_t pde_no = VA_t(MMAP_USER_BASE).pdx ; pde_no < VA_t(MMAP_USER_TOP).pdx ; pde_no++) {
		/* kullanilmayan pagetablelari atla */
		if (!t->pgdir.pgdir->e[pde_no].present)
			continue;
		/* kullanilan butun pageleri serbest birak */
		for (uint32_t pte_no = 0 ; pte_no < 1024 ; pte_no++) {
			ASSERT(t->pgdir.pgtables[pde_no]);
			VA_t va(pde_no, pte_no);
			if (t->pgdir.pgtables[pde_no]->e[pte_no].present) {
                /* simdilik page paylasimi yok, silince refcount=0 olmali */
				int i = t->pgdir.page_remove(va, 0);
				ASSERT(i == 0);
			}
		}
		/* free pages page table */
		t->pgdir.pde_free(pde_no);
	}

	cr3_reload();

	t->pgdir.count_stack = 0;
	t->pgdir.count_program = 0;
}

// TODO: bu fonksiyon kaldirilabilir
static int task_alloc(Task **t) {
	ASSERT(!(eflags_read() & FL_IF));

	*t = (Task*)kmalloc(sizeof(Task));
	if (*t == NULL)
		return -1;
	return 0;
}

void task_free(Task *t) {
	ASSERT(!(eflags_read() & FL_IF));
	ASSERT(t == task_curr);
	// printf(">> task %08x: free task %08x\n", task_curr ? task_curr->id : 0, t->id);

	ipc_task_free(t);

	uint32_t count_brk = (t->pgdir.end_brk - t->pgdir.start_brk) / 0x1000;
	ASSERT(t->pgdir.count_stack + t->pgdir.count_program + count_brk == t->pgdir.count_user);
	task_free_vm_user(t);
	ASSERT(t->pgdir.count_user == 0);

	/*
	 * kernel stack ve pagedir daha sonra zombie list uzerinden silinecek.
	 * task structini bulundugu listeden cikar, zombie olarak isaretle ve
	 * task_zombie_list'e ekle.
	 */
	t->list_node.__list->erase(t->list_node.get_iterator());
	t->state = Task::State_zombie;
	task_zombie_list.push_back(&t->list_node);

	if (t->alarm_list_node.__list)
		t->alarm_list_node.__list->erase(&t->alarm_list_node);
}

void free_zombie_tasks() {
	ASSERT(!(eflags_read() & FL_IF));

	while (task_zombie_list.size() > 0) {
		Task *t = task_zombie_list.front();
		if (t == task_curr)
			return;
		task_zombie_list.pop_front();
		task_free_kernel_stack(t);
		task_delete_vm(t);
		t->state = Task::State_not_runnable;;
		t->free = 1;
		kfree(t);
	}
}

void task_create(void* program_addr, const char* cmd, int priority) {
	ASSERT(!(eflags_read() & FL_IF));

	int err, r;
	Task *task;
	PageDirInfo *pgdir;
	Trapframe registers;
	Page *p;
	uint32_t va;

	if (task_alloc(&task) < 0)
		goto bad_task_create;

	memset(task, 0, sizeof(Task));
	task->init();

	task->id = next_task_id++;
	ASSERT( task_id_ht.put(&task->id_hash_node) == 0);

	if (task->id == 1)
		task->parent = NULL;
	else {
		task->parent = task_id_ht.get(1);
		ASSERT( task->parent );
	}
	task->state = Task::State_not_runnable;
	task->run_count = 0;
	task->priority = priority;

	memset(&registers, 0, sizeof(registers));
	registers.ds = GD_UD | 3;
	registers.es = GD_UD | 3;
	registers.ss = GD_UD | 3;
	registers.cs = GD_UT | 3;

	/* user modda interruptlar aktif */
	registers.eflags = 0;
	registers.eflags |= FL_IF;

	// memory/virtual.cpp
	extern PageDirInfo kernel_dir;
	//

	if (task->id == 1)
		pgdir = &kernel_dir;
	else
		pgdir = &task_id_ht.get(1)->pgdir;

	err = task_setup_vm(task, pgdir);

	if (err < 0)
		goto bad_task_create;
	// printf(">> task_setup_vm OK\n");

	// FIXME: --
	extern PageDirInfo *pgdir_curr;
	if (pgdir_curr) {
		task->pgdir.link_pages(pgdir_curr, MMAP_KERNEL_STACK_BASE,
							   MMAP_KERNEL_STACK_TOP, PTE_P | PTE_W);
	}

	if (task_curr) {
		task->pgdir.link_pages(&task_curr->pgdir, MMAP_KERNEL_STACK_BASE,
							   MMAP_KERNEL_STACK_TOP, PTE_P | PTE_W);
	}

	err = load_icode(task, &registers, (uint32_t)program_addr, cmd);
	if (err < 0)
		goto bad_task_create;
	// printf(">> load_icode OK\n");

	/* process icin kernel stack */
	r = tmp_page_alloc_map(&p, &va, PTE_P | PTE_W);
	ASSERT(r == 0);
	r = task->pgdir.page_insert(p, MMAP_KERNEL_STACK_BASE, PTE_P | PTE_W);
	ASSERT(r == 2);

	memcpy((void*)va2kaddr(va+0x1000-sizeof(Trapframe)), &registers, sizeof(Trapframe));

	task->k_esp = va2kaddr(MMAP_KERNEL_STACK_TOP) - sizeof(Trapframe);

	task->k_eip = (uint32_t)task_first_run;
	tmp_page_free(va);
	/* */

	task->shared_mem_list.init();

	task->state = Task::State_running;
	ASSERT(task->list_node.is_free());

	add_to_runnable_list(task);
	printf(">> task %08x created\n", task->id);
	return;

bad_task_create:
	PANIC("task create error");
}

void task_init() {
	task_id_ht.init(mem_task_id_ht, sizeof(mem_task_id_ht));
	next_task_id++;
	task_zombie_list.init();

	memset(&task0, 0, sizeof(Task));

	printf(">> task_init OK\n");
}

asmlink void sys_getpid(void) {
	ASSERT(task_curr);
	return set_return(task_curr->registers(), task_curr->id);
}


// TODO: registers_users'i kaldir
// TODO: fork kurallari duzenlenmeli (neler kopyalanacak, neler kopyalanmayacak?)
asmlink void sys_fork() {
	uint32_t eflags = eflags_read();
	cli();

	int r;
	Task *t;
	uint32_t eip;
	int e = 0; // error (bad_fork_* icin)

	/* debug only */
	uint32_t mem_before_setup_vm = 0,
		mem_before_copy_pages = 0,
		mem_before_kernel_stack = 0;
	/* */

	if (task_alloc(&t) < 0)
		goto bad_fork_task_alloc;

	*t = *task_curr;
	t->init();

	t->parent = task_curr;
	t->state = Task::State_not_runnable;
	t->run_count = 0;

	mem_before_setup_vm = mem_free();
	r = task_setup_vm(t, &task_curr->pgdir);
	if (r < 0)
		goto bad_fork_setup_vm;

	/* user adres uzayini kopyala */
	mem_before_copy_pages = mem_free();
	r = t->pgdir.copy_pages(&task_curr->pgdir, MMAP_USER_BASE, MMAP_USER_SHARED_MEM_BASE);
	if (r < 0)
		goto bad_fork_copy_vm_user;

	/* shared memory kismini kopyalama, shm_fork fonksiyonu kopyalayacak  */

	r = t->pgdir.copy_pages(&task_curr->pgdir, MMAP_USER_SHARED_MEM_TOP , MMAP_USER_TOP);
	if (r < 0)
		goto bad_fork_copy_vm_user;

	t->pgdir.count_program = task_curr->pgdir.count_program;
	t->pgdir.count_stack = task_curr->pgdir.count_stack;
	/* */

	/* signal */
	/*
	 * Signal handler fonksiyonundan fork yapilirsa ne yapilmali ?
	 */
	/* */

	r = ipc_fork(t);
	if (r < 0)
		goto bad_fork_ipc;

	/*
	 * kernel stackini kopyala
	 * child return degeri 0 (registers stackde bulunuyor)
	 */
	set_return(task_curr->registers(), 0);
	mem_before_kernel_stack = mem_free();
	r = task_copy_kernel_stack(t);
	if (r < 0)
		goto bad_fork_copy_kernel_stack;
	/* */

	/* burasi 2 kere calisiyor */
	eip = read_eip();
	if (eip == 1)
		return;

	t->k_eip = eip;
	t->k_esp = esp_read();
	t->k_ebp = ebp_read();
	t->time_start = jiffies;
	t->id = next_task_id++;
	ASSERT( task_id_ht.put(&t->id_hash_node) == 0);
	/* child listesine ekle */
	ASSERT( task_curr->childs.push_back(&t->childlist_node) );
	/* runnable listesine ekle */
	t->state = Task::State_running;
	add_to_runnable_list(t);

	/* paret incin return degeri child id */
	set_return(task_curr->registers(), t->id);

	return;

bad_fork_copy_kernel_stack:
	if (e++ == 0)
		printf("!! bad_fork_copy_kernel_stack\n");
	task_free_kernel_stack(t);
	ASSERT(mem_free() == mem_before_kernel_stack);
bad_fork_ipc:
	// TODO: --
bad_fork_copy_vm_user:
	if (e++ == 0)
		printf("!! bad_fork_copy_vm_user\n");
	task_free_vm_user(t);
	ASSERT(mem_free() == mem_before_copy_pages);
bad_fork_setup_vm:
	if (e++ == 0)
		printf("!! bad_fork_setup_vm\n");
	task_delete_vm(t);
	ASSERT(mem_free() == mem_before_setup_vm);
	kfree(t);
	t = NULL;
bad_fork_task_alloc:
	if (e++ == 0)
		printf("!! bad_fork_task_alloc\n");
	ASSERT(t == NULL);
// bad_fork:
	eflags_load(eflags);
	return set_return(task_curr->registers(), -1);
}

/** sadece task create tarafindan kullaniliyor */
void __task_first_run() {
	asm("task_first_run:"
		"pop %eax\n\t"
		"jmp trapret");
	PANIC("--");
}

/** ilk processi baslatan fonksiyon */
void run_first_task() {
	task_curr = task_id_ht.get(1);
	task_curr->k_eip = 0; // user modda baslayacak
	cr3_load(task_curr->pgdir.pgdir_pa);
	esp_load(task_curr->k_esp);

	asm("jmp trapret");
	PANIC("--");
}

void switch_to_task(Task *newtask) {
	ASSERT(!(eflags_read() & FL_IF));
	ASSERT(task_curr);
	// printf(">> %d switch to %d\n", task_curr->id, newtask->id);

	/* eski processin bilgilerini kaydediyoruz */
	task_curr->k_esp = esp_read();
	task_curr->k_ebp = ebp_read();
	/* */

	task_curr = newtask;
	task_curr->run_count++;

/*
 * UYARI: stack degistirilirken ve degistirildikten sonra yerel degiskenler
 * kullanilmamali. Degiskenler stack uzerinden kullaniliyor olabilir.
 */

	/* task create ve fork sonrasi ilk calisma icin */
	if (newtask->k_eip) {
		asm volatile(
			"movl (%0), %%eax\n\t"
			"movl %1, %%esp\n\t" // esp_load
			"movl %2, %%ebp\n\t" // ebp load
			"movl %3, %%cr3\n\t" // cr3_load
			"pushl $1\n\t" // read_eip() icin return degeri 1
			"pushl %%eax\n\t" // ret ile yuklenecek program counter
			"movl $0, (%0)\n\t"
			"ret"
			:
			: "r"(&task_curr->k_eip),
			  "r"(task_curr->k_esp),
			  "r"(task_curr->k_ebp),
			  "r"(task_curr->pgdir.pgdir_pa)
			: "eax"
			);
	}

	/* yeni process'in page_directory'si ve stack'i yukleniyor */
	asm volatile(
		"movl %0, %%cr3\n\t" // cr3_load
		"movl %1, %%esp\n\t" // esp_load
		"movl %2, %%ebp\n\t" // ebp_load
		:
		: "r"(task_curr->pgdir.pgdir_pa),
		  "r"(task_curr->k_esp),
		  "r"(task_curr->k_ebp)
		);

	/* bu satirdan yeni process ile devam ediliyor */
}
