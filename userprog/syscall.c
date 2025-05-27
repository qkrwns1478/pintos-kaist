#include <stdio.h>  		    // printf
#include <syscall-nr.h>         // enum
#include "threads/interrupt.h"  // intr_frame
#include "threads/init.h"       // shutdown_power_off()
#include "threads/vaddr.h"      // is_user_vaddr(), pg_round_down()
#include "threads/thread.h"     // struct thread, thread_exit(), thread_current()
#include "threads/loader.h"     
#include "threads/flags.h"
#include "threads/palloc.h"
#include "userprog/gdt.h"
#include "userprog/syscall.h"   //syscaa_handler prototype 등
#include "intrinsic.h" 			// for hex_dump
#include "devices/input.h"      // input_getc()
#include "filesys/filesys.h"    // filesys_* function (if used)
#include "filesys/file.h"		// struct file, file_read, file_write
#include "filesys/fat.h"		// cluster_t 
#include "lib/kernel/console.h"








void syscall_entry (void);
void syscall_handler (struct intr_frame *);

void check_address(const void *addr);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	uint64_t syscall_num = f->R.rax;
    
	// printf("[DEBUG] syscall number: %lld\n", syscall_num);

	switch (syscall_num) {
	case SYS_FILESIZE:
		f->R.rax = sys_filesize((int)f->R.rdi);
		break;

	case SYS_CREATE: {
		const char *file = f->R.rdi;
		unsigned size = f->R.rsi;

		if (file == NULL)
			sys_exit(-1);

		check_address(file); // 문자열 포인터 검사하기
		

		
		f->R.rax = filesys_create(file, size);
		break;
	}

	case SYS_OPEN:{
		const char *file = f->R.rdi;

		check_address(file);      // 파일 이름 포인터 검사
    	f->R.rax = sys_open((const char *)f->R.rdi);
        break;
	}

	case SYS_CLOSE:
    	sys_close((int)f->R.rdi);
    	break;
	
	case SYS_WRITE:{
		int fd = f->R.rdi;
		const void *buffer = f->R.rsi;
		unsigned size = f->R.rdx;

		check_address(buffer);  // 쓰기 대상 버퍼 검사
		f->R.rax = sys_write((int)f->R.rdi, (const void *)f->R.rsi, (unsigned)f->R.rdx);
		break;
	}

	case SYS_EXIT: 
		sys_exit((int)f->R.rdi);
		break;
	
	case SYS_READ:{
		int fd = f->R.rdi;
		const void *buffer = f->R.rsi;
		unsigned size = f->R.rdx;

		check_address(buffer); // 읽기 대상 버퍼 검사
		f->R.rax = sys_read((int)f->R.rdi, (void *)f->R.rsi, (unsigned)f->R.rdx);
		break;
	}

	case SYS_FORK:{
		const char *name = f->R.rdi;

		check_address(name); // user 주소인지 pml4에 매핑되어 있는지 확인

		f->R.rax = fork(f->R.rdi, f);
		break;
	}
	default:
		sys_exit(-1);
		break;
	}	
}

int sys_filesize(int fd) {
	struct thread *curr = thread_current();

	if (fd < 2 || fd >= FDCOUNT_LIMIT || curr->fd_table[fd] == NULL)
		return -1;
	return file_length(curr->fd_table[fd]);
}

int
sys_open (const char *file) {
	struct thread *curr = thread_current();

	// if (file == NULL || !is_user_vaddr(file))
	//  // 유효하지 않은 사용자 포인터 예외 처리
	// 	return -1;

	check_address(file); // 췤 어드레스 활용해서 NULL, user space, 매핑 여부 check


	struct file *f = filesys_open(file); // 파일 열기
	if (f == NULL)
		return -1;

	// 현재 프로세스의 fd_table에서 빈칸에 넣고 fd 반환
	// int fd = curr->next_fd++; // fd_table 공간이 없으면 하나 할당(초기화는 process_exec()에서 했다고 가정)
	// curr->fd_table[fd] = f;

	for (int fd = 2; fd < FDCOUNT_LIMIT; fd++) {
		if (curr->fd_table[fd] == NULL) {
			curr->fd_table[fd] = f;
			if (fd >= curr->next_fd)
				curr->next_fd = fd + 1;
			return fd;
		}
	}
	file_close(f); // 여유 fd가 없을 경우 자원 반환
	return -1;
}

int
sys_write(int fd, const void *buffer, unsigned size) {
	// if (buffer == NULL || !is_user_vaddr(buffer)) // buffer가 NULL이거나 커널 주소를 가리키면 종료(예외처리shit)
	// 	return -1;

	check_address(buffer); // 췤 어드레스 활용해서 NULL, user space, 매핑 여부 check

	struct thread *curr = thread_current(); //현재 실행중인 스레드 가져오기

	if (fd == 1) { // 표준 출력에 대한 요청인지 확인(STDOUT)
		putbuf(buffer, size); // 콘솔 출력 버퍼에 데이터 복사
		return size;		  // 출력한 바이트 수 반환
	}
	//예외 처리 shit
	// fd가 0또는 1보다 작으면(stdin, stdout이 아닌 잘못된 값)
	// fd가 상한을 넘는 값이면 (배열 인덱스 초과 위험)
	// fd_table에 아무 파일도 열려 있지 않으면(NULL 접근 위험)
	if (fd < 2 || fd >= FDCOUNT_LIMIT || curr->fd_table[fd] == NULL) { 
		return -1;
	}

	return file_write(curr->fd_table[fd], buffer, size);
}

void 
check_valid_buffer(const void *buffer, unsigned size) {
	uint8_t *start = (uint8_t *)buffer;
	for (unsigned i = 0; i < size; i++) {
			check_address(start + i);
	}
}

void
sys_exit(int status) {
	struct thread *curr = thread_current();
	curr->exit_status = status;
	// printf("%s: exit(%d)\n", curr->name, status);  //메세지 출력은 process.c 의 process_exit 에 구현해버림
	thread_exit();
}

void
sys_close(int fd) {
	struct thread *curr = thread_current();

	if (fd < 2 || fd >= curr->next_fd || curr->fd_table[fd] == NULL)
		return;

	file_close(curr->fd_table[fd]);
	curr->fd_table[fd] = NULL;
}

void
check_address(const void *addr) {
	struct thread *curr = thread_current();
	// 포인터가 NULL 인지 확인
	if (addr == NULL) {
		// printf("error 1\n");
		sys_exit(-1);
	}
	// 포인터가 유저 주소 범위 안에 있는지 확인하기
	if (!is_user_vaddr(addr)) {
		// printf("error 2\n");
		sys_exit(-1);
	}
	if (pml4_get_page(curr->pml4, addr) == NULL) {
		// printf("error 3\n");
		sys_exit(-1);
	}
	
}

int
sys_read(int fd, void *buffer, unsigned size){
	// check_valid_buffer(buffer, size); // 췤
	check_address(buffer);

	struct thread *curr = thread_current();
	if (fd == 0) { //STDIN
		uint8_t *buf = (uint8_t *)buffer;
		for (unsigned i = 0; i < size; i++) {
			buf[i] = input_getc();
		}
		return size;
	}	

	// fd 유효성 확인하기
	if (fd < 2 || fd >= FDCOUNT_LIMIT || curr->fd_table[fd] == NULL) {
		return -1;
	}
	// 열린 파일에서 읽기
	return file_read(curr->fd_table[fd], buffer, size);
}

tid_t
fork(const char *thread_name, struct intr_frame *f) {
	return process_fork(thread_name, f);
}