#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <signal.h>

#define MAXPROC 10
#define MAX_LINE_LEN 1024
#define MAX_CMD_LEN 512
#define MAX_PATH_LEN 256

#define TERM_TIMEOUT_US 3000000
#define POLL_INTERVAL_US  100000

typedef struct {
	char command[MAX_CMD_LEN];
	char stdin_path[MAX_PATH_LEN];
	char stdout_path[MAX_PATH_LEN];
	pid_t child_pid;
	int is_child_active;
} ChildProcess;

void exit_with_perror(const char* message) {
	perror(message);
	exit(1);
}

void exit_with_error(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	exit(1);
}

void ensure_file_exists(const char* path) {
	if (access(path, F_OK) == 0) {
		return;
	}

	exit_with_perror("Файл не найден");
}

void ensure_valid_path_provided(const char* path) {
	if (path == NULL || path[0] != '/') {
		exit_with_error("Пути к файлам должны быть полными");
	}
}

char* log_file_path = "/tmp/myinit.log";
int log_fd;

void init_log() {
	log_fd = open(log_file_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (log_fd < 0) {
		exit_with_perror("Не получилось создать лог-файл");
	}
}

void log_message(const char* format, ...) {
	va_list args;
	va_start(args, format);
	if (log_fd >= 0) {
		vdprintf(log_fd, format, args);
		fsync(log_fd);
	}
	va_end(args);
}

static ChildProcess child_processes[MAXPROC];
static int child_processes_count = 0;

int parse_config_line(const char* line, ChildProcess* child) {
	if (!line || !child) {
		return -1;
	}

	char line_copy[MAX_LINE_LEN];
	strncpy(line_copy, line, MAX_LINE_LEN - 1);
	line_copy[MAX_LINE_LEN - 1] = '\0';

	char *newline = strchr(line_copy, '\n');
	if (newline) {
		*newline = '\0';
	}
	char *carriage = strchr(line_copy, '\r');
	if (carriage) {
		*carriage = '\0';
	}

	char *p = line_copy;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p == '\0' || *p == '#') {
		return -1;
	}

	char *tokens[64];
	int token_count = 0;
	char *token = strtok(p, " \t");
	while (token && token_count < 63) {
		tokens[token_count++] = token;
		token = strtok(NULL, " \t");
	}

	if (token_count < 3) {
		exit_with_error("Ошибка: неправильный формат строки конфигурации (недостаточно полей): %d\n", token_count);
	}

	strncpy(child->stdout_path, tokens[token_count - 1], MAX_PATH_LEN - 1);
	strncpy(child->stdin_path, tokens[token_count - 2], MAX_PATH_LEN - 1);

	child->command[0] = '\0';
	for (int i = 0; i < token_count - 2; ++i) {
		if (i > 0) {
			strncat(child->command, " ", MAX_CMD_LEN - 1);
		}
		strncat(child->command, tokens[i], MAX_CMD_LEN - 1);
	}

	ensure_valid_path_provided(tokens[0]);
	ensure_file_exists(tokens[0]);

	ensure_valid_path_provided(child->stdin_path);
	ensure_file_exists(child->stdin_path);

	ensure_valid_path_provided(child->stdout_path);

	child->child_pid = 0;
	child->is_child_active = 0;

	return 0;
}

void read_config_lines(const char *config_path) {
	FILE *fp = fopen(config_path, "r");
	if (!fp) {
		exit_with_perror("Ошибка чтения конфигурационного файла");
	}

	char line[MAX_LINE_LEN];
	child_processes_count = 0;

	while (fgets(line, sizeof(line), fp) && child_processes_count < MAXPROC) {
		if (parse_config_line(line, &child_processes[child_processes_count]) == 0) {
			log_message("Обработана строка: [%s] stdin=[%s] stdout=[%s]\n", 
				child_processes[child_processes_count].command,
				child_processes[child_processes_count].stdin_path,
				child_processes[child_processes_count].stdout_path);
			child_processes_count++;
		}
	}

	fclose(fp);

	if (child_processes_count == 0) {
		exit_with_error("Ошибка: не найдено ни одной правильно заполненной строки конфигурационного файла");
	}

	log_message("Конфигурация загружена: %d строк обработано\n", child_processes_count);
}

char* config_file_path = NULL;

void read_config(int argc, char** argv) {
	if (argc < 2) {
		exit_with_error("Ошибка: требуется полный путь к конфигурационному файлу дочерних процессов");
	}

	config_file_path = argv[1];
	ensure_valid_path_provided(config_file_path);
	ensure_file_exists(config_file_path);

	read_config_lines(config_file_path);
}

void start_child_from_entry(int idx) {
	ChildProcess* c = &child_processes[idx];
	pid_t pid = fork();

	if (pid < 0) {
		log_message("Ошибка: не удалось выполнить fork() для процесса с id: %d\n", idx);
		return;
	}

	if (pid == 0) {
		// === Дочерний процесс: перенаправление ===

		// stdin
		int fd_in = open(c->stdin_path, O_RDONLY);
		if (fd_in < 0) {
			perror("Ошибка перенаправления потока ввода запущенного процесса");
			_exit(127);
		}
		dup2(fd_in, STDIN_FILENO);
		close(fd_in);

		// stdout (+ stderr)
		int fd_out = open(c->stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd_out < 0) {
			perror("Ошибка перенаправления потока вывода запущенного процесса");
			_exit(127);
		}
		dup2(fd_out, STDOUT_FILENO);
		dup2(fd_out, STDERR_FILENO);
		close(fd_out);

		struct rlimit rl;
		if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
			for (int fd = 3; fd < (int)rl.rlim_max; ++fd) {
				close(fd);
			}
		}
		// Exec команды
		execl("/bin/sh", "sh", "-c", c->command, (char*)NULL);
		perror("Ошибка запуска команды в запущенном процессе");
		_exit(127);
	}

	// === Родитель ===
	c->child_pid = pid;
	c->is_child_active = 1;
	log_message("Дочерний процесс %d запущен успешно: pid=%d, cmd=%s\n", idx, pid, c->command);
}

void start_all_children() {
	for (int i = 0; i < child_processes_count; i++) {
		if (!child_processes[i].is_child_active) {
			start_child_from_entry(i);
		}
	}
}

ChildProcess* find_child_process_by_pid(pid_t pid) {
	for (int i = 0; i < child_processes_count; ++i) {
		if (child_processes[i].child_pid == pid) {
			return &child_processes[i];
		}
	}
	return NULL;
}

void handle_finished_children() {
	int status;
	pid_t pid;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		ChildProcess* child = find_child_process_by_pid(pid);
		if (child == NULL) {
			log_message("Предупреждение: неизвестный дочерний процесс. pid: %d\n", pid);
			continue;
		};
		int idx = child - child_processes;
		if (WIFEXITED(status)) {
			log_message("Дочерний процесс %d (pid: %d) завершился со статусом %d\n", idx, pid, WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			log_message("Дочерний процесс %d (pid: %d) был убит сигналом %d\n", idx, pid, WTERMSIG(status));
		}
		child->is_child_active = 0;
		log_message("Перезапускаю дочерний процесс %d: %s\n", idx, child->command);
		start_child_from_entry(idx);
	}
}

static volatile sig_atomic_t reload_requested = 0;

void sighup_handler(int sig) {
	(void) sig;
	reload_requested = 1;
}

void terminate_all_children() {
	for (int i = 0; i < child_processes_count; ++i) {
		if (child_processes[i].is_child_active == 1 && child_processes[i].child_pid > 0) {
			kill(child_processes[i].child_pid, SIGTERM);
		}
	}

	int elapsed_us = 0;
	while (elapsed_us < TERM_TIMEOUT_US) {
		int all_done = 1;
		for (int i = 0; i < child_processes_count; ++i) {
			if (child_processes[i].is_child_active && child_processes[i].child_pid > 0) {
				int status;
				pid_t res = waitpid(child_processes[i].child_pid, &status, WNOHANG);
				if (res > 0) {
					if (WIFEXITED(status)) {
						log_message("Процесс %d (pid %d) завершился со статусом %d\n", i, child_processes[i].child_pid, WEXITSTATUS(status));
					} else if (WIFSIGNALED(status)) {
						log_message("Процесс %d (pid %d) убит сигналом %d\n", i, child_processes[i].child_pid, WTERMSIG(status));
					}
					child_processes[i].is_child_active = 0;
					child_processes[i].child_pid = 0;
				} else if (res == 0) {
					all_done = 0;
				} else if (errno == ECHILD) {
					child_processes[i].is_child_active = 0;
					child_processes[i].child_pid = 0;
				}
			}
		}
		if (all_done) {
			break;
		}
		usleep(POLL_INTERVAL_US);
		elapsed_us += POLL_INTERVAL_US;
	}

	for (int i = 0; i < child_processes_count; ++i) {
		if (child_processes[i].is_child_active && child_processes[i].child_pid > 0) {
			log_message("Процесс %d (pid %d) игнорирует SIGTERM. Отправляю SIGKILL.\n", 
				i, child_processes[i].child_pid);
			kill(child_processes[i].child_pid, SIGKILL);
		}
	}

	usleep(100000);
	for (int i = 0; i < child_processes_count; ++i) {
		if (child_processes[i].child_pid > 0) {
			int status;
			waitpid(child_processes[i].child_pid, &status, 0);
			child_processes[i].is_child_active = 0;
			child_processes[i].child_pid = 0;
		}
	}
	log_message("Все дочерние процессы завершены.\n");
}

int main(int argc, char** argv) {
	struct rlimit flim;
	char *message = "Процесс myinit запущен\n";

	if (getppid() != 1) {
		signal(SIGTTOU, SIG_IGN);
		signal(SIGTTIN, SIG_IGN);
		signal(SIGTSTP, SIG_IGN);

		if (fork() != 0) {
			exit(0);
		}

		setsid();
	}

	getrlimit(RLIMIT_NOFILE, &flim);
	for (int fd = 0; fd < (int) flim.rlim_max; ++fd) {
		close(fd);
	}

	if (chdir("/") != 0) {
		return 1;
	}

	init_log();
	log_message(message);

	read_config(argc, argv);
	start_all_children();

	signal(SIGHUP, sighup_handler);

	while (1) {
		if (reload_requested) {
			reload_requested = 0;
			log_message("Получен сигнал SIGHUP. Перезагружаю конфигурацию\n");
			terminate_all_children();
			read_config_lines(config_file_path);
			start_all_children();
		}

		handle_finished_children();
		usleep(100*1000);
	}

	return 0;
}
