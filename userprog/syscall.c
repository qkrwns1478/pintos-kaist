// 핀토스에서 시스템 콜을 구현. | 사용자 프로그램이 커널과 상호작용하기 위한 인터페이스 제공.

void syscall_entry (void); // 시스템 콜 진입 지점 (어셈블리어로 작성됨)
void syscall_handler (struct intr_frame *); // 시스템 콜 핸들러 함수

#define MSR_STAR 0xc0000081         /* Segment selector msr */ // 세그먼트 셀렉터 MSR 레지스터
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */ // 롱 모드 시스템 콜 타겟 주소 MSR 레지스터
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */ // 시스템 콜 실행 시 마스킹할 EFLAGS 레지스터 비트들 MSR 레지스터

void halt(void); // 시스템 종료 시스템 콜
void exit(int status); // 현재 사용자 프로그램 종료 시스템 콜
tid_t fork(const char *thread_name, struct intr_frame *f); // 현재 프로세스 복제 시스템 콜
int exec(const char *file); // 현재 프로세스를 새로운 실행 파일로 교체 시스템 콜
int wait(tid_t pid); // 자식 프로세스가 종료될 때까지 대기 시스템 콜

void
syscall_init (void) {
	// 시스템 콜 초기화 함수
	// MSR 레지스터에 커널 및 사용자 코드 세그먼트 정보를 설정합니다.
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	// MSR 레지스터에 시스템 콜 엔트리 포인트 주소를 설정합니다.
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	// 시스템 콜 진입 시 유저 스택에서 커널 스택으로 전환될 때까지 인터럽트 서비스 루틴이
	// 어떤 인터럽트도 처리해서는 안 됩니다. 따라서 특정 EFLAGS 비트를 마스킹합니다.
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	// 메인 시스템 콜 핸들러 인터페이스
	// 인터럽트 프레임에서 사용자 스택 포인터를 현재 스레드 구조체에 저장합니다.
	thread_current()->user_rsp = f->rsp;

	// RAX 레지스터에 담긴 시스템 콜 번호에 따라 해당 함수를 호출합니다.
	switch (f->R.rax) {
		case SYS_HALT: // 시스템 종료
			halt();
			break;
		case SYS_EXIT: // 현재 프로그램 종료 (종료 상태 코드 전달)
			exit(f->R.rdi);
			break;
		case SYS_FORK: // 현재 프로세스 복제 (새 프로세스 이름 및 인터럽트 프레임 전달)
			f->R.rax = fork((const char *)f->R.rdi, f);
			break;
		case SYS_EXEC: // 새로운 실행 파일로 프로세스 교체 (파일 이름 전달)
			f->R.rax = exec((const char *)f->R.rdi);
			break;
		case SYS_WAIT: // 특정 자식 프로세스 종료 대기 (자식 프로세스 ID 전달)
			f->R.rax = wait((tid_t)f->R.rdi);
			break;
		default: // 구현되지 않은 시스템 콜
			printf ("Unimplemented system call: %lld\n", f->R.rax);
			exit(-1);
	}
}

/* Shuts down the machine. */
void halt(void) {
	// 시스템 종료 함수 (power_off() 함수 호출)
	power_off();
}

/* Terminates the current user program with the specified status. */
void exit(int status) {
	// 현재 사용자 프로그램을 지정된 상태(status)로 종료하는 함수
	struct thread *cur = thread_current(); // 현재 스레드 정보를 가져옴
	cur->exit_status = status; // 종료 상태를 스레드 구조체에 저장
	printf("%s: exit(%d)\n", cur->name, status); // 종료 메시지 출력
	thread_exit(); // 스레드 종료 처리
}

/* Creates a copy of the current process. */
tid_t fork(const char *thread_name, struct intr_frame *f) {
	// 현재 프로세스의 복사본을 생성하는 함수
	// 실제 복사 작업은 process_fork 함수에서 수행함.
	return process_fork(thread_name, f);
}

/* Replaces the current process with a new executable. */
int exec(const char *file) {
	// 현재 프로세스를 새로운 실행 파일(file)로 교체하는 함수
	char *fn_copy = palloc_get_page(0); // 파일 이름 복사를 위한 페이지 할당
	if (fn_copy == NULL) // 페이지 할당 실패 시
		return -1; // 오류 반환

	strlcpy(fn_copy, file, PGSIZE); // 파일 이름을 복사
	if (process_exec(fn_copy) == -1) // process_exec 함수를 호출하여 실행 파일 로드 및 실행
		return -1; // 실행 실패 시 오류 반환

	NOT_REACHED(); // process_exec가 성공하면 이 지점에 도달하지 않습니다.
	return 0;
}

/* Waits for a child process to terminate. */
int wait(tid_t pid) {
	// 특정 자식 프로세스(pid)가 종료될 때까지 대기하는 함수
	// 실제 대기 작업은 process_wait 함수에서 수행함.
	return process_wait(pid);
}
