#include <iostream>
#include <vector>
#include <random>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <cstring>
#include <cmath>
#include <signal.h>
#include <string>

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

#define SEM_MAIN 0
#define SEM_PLAYER_START 1

struct MoveMsg {
    long mtype;
    int player_id;
    int move;
};

int semid = -1;
int msqid = -1;
std::vector<pid_t> playerPids;
int n;
std::string moves[3] = {"камень", "ножницы", "бумага"};

// функция wait для семафора system v
void sem_wait_sysv(int semnum) {
    struct sembuf arg{
        .sem_num = static_cast<short>(semnum),
        .sem_op = -1,
        .sem_flg = 0
    };
    if (semop(semid, &arg, 1) == -1) {
        perror("semop wait");
        exit(1);
    }
}

// функция post для семафора system v
void sem_post_sysv(int semnum) {
    struct sembuf arg{
        .sem_num = static_cast<short>(semnum),
        .sem_op = 1,
        .sem_flg = 0
    };
    if (semop(semid, &arg, 1) == -1) {
        perror("semop signal");
        exit(1);
    }
}

int referee(int move1, int move2) {
    if (move1 == move2) {
        return 0;
    } else if (move1 == 0 && move2 == 1 || move1 == 1 && move2 == 2 || move1 == 2 && move2 == 0) {
        return 1;
    }
    return 2;
}

void playerProcess(int id) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, 2);

    while (true) {
        sem_wait_sysv(SEM_PLAYER_START + id - 1);
        sem_wait_sysv(SEM_MAIN);

        int move = distrib(gen);
        MoveMsg msg;
        msg.mtype = 1;
        msg.player_id = id - 1;
        msg.move = move;

        if (msgsnd(msqid, &msg, sizeof(msg) - sizeof(long), 0) == -1) {
            perror("msgsnd");
            exit(1);
        }

        sem_post_sysv(SEM_MAIN);
    }
    exit(0);
}

void cleanup() {
    for (pid_t pid : playerPids) {
        kill(pid, SIGTERM);
    }

    for (pid_t pid : playerPids) {
        waitpid(pid, nullptr, 0);
    }

    if (semid != -1) {
        semctl(semid, 0, IPC_RMID);
    }

    if (msqid != -1) {
        msgctl(msqid, IPC_RMID, nullptr);
    }

    exit(0);
}

void handle_sigint(int sig) {
    std::cout << "Получен SIGINT, завершаю турнир и очищаю ресурсы..." << std::endl;
    cleanup();
}

int main() {
    struct sigaction sa{};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, nullptr);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distribPlayers(2, 101);
    n = distribPlayers(gen);
    std::cout << "Количество игроков в турнире: " << n << std::endl;

    int totalSems = SEM_PLAYER_START + n;
    semid = semget(IPC_PRIVATE, totalSems, IPC_CREAT | 0666);
    if (semid < 0) { 
        perror("semget");
        return 1;
    }
    
    union semun arg;
    arg.val = 1;
    semctl(semid, SEM_MAIN, SETVAL, arg);

    for (int i = 0; i < n; i++) {
        arg.val = 0;
        semctl(semid, SEM_PLAYER_START + i, SETVAL, arg);
    }

    msqid = msgget(IPC_PRIVATE, IPC_CREAT | 0666);
    if (msqid < 0) { 
        perror("msgget");
        return 1;
    }

    std::vector<bool> is_in_game(n, true);
    std::vector<int> opponent(n, -1);
    std::vector<int> round_winners(n);

    int winners_count = 0;
    int tournament_winner = -1;

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            return 1;
        }

        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            playerProcess(i + 1);
        }

        playerPids.push_back(pid);
    }

    int numRounds = static_cast<int>(std::ceil(std::log2(n)));
    std::vector<int> activeStudents(n);
    for (int i = 0; i < n; i++) activeStudents[i] = i;

    for (int round = 1; round <= numRounds; round++) {
        std::cout << "\nРаунд " << round << std::endl;
        winners_count = 0;

        if (activeStudents.size() % 2 == 1) {
            int idx = activeStudents.back();
            activeStudents.pop_back();
            std::cout << "Студент " << idx + 1 << " проходит в следующий раунд" << std::endl;
            round_winners[winners_count++] = idx;
        }

        int matchCount = activeStudents.size() / 2;
        for (int m = 0; m < matchCount; m++) {
            int p1 = activeStudents[2 * m];
            int p2 = activeStudents[2 * m + 1];
            std::cout << "Матч между " << p1 + 1 << " и " << p2 + 1 << std::endl;

            sem_post_sysv(SEM_PLAYER_START + p1);
            sem_post_sysv(SEM_PLAYER_START + p2);

            bool decided = false;
            while (!decided) {

                MoveMsg msg1, msg2;
                // Так как процесс получения блокирующий, то мы можем получить оба сообщения без семафоров
                if (msgrcv(msqid, &msg1, sizeof(msg1) - sizeof(long), 0, 0) == -1) {
                    perror("msgrcv");
                    cleanup();
                }
                if (msgrcv(msqid, &msg2, sizeof(msg2) - sizeof(long), 0, 0) == -1) {
                    perror("msgrcv");
                    cleanup();
                }

                int c1 = msg1.move;
                int c2 = msg2.move;
                std::cout << "   Игрок " << msg1.player_id + 1 << " выбрал " << moves[c1] << std::endl;
                std::cout << "   Игрок " << msg2.player_id + 1 << " выбрал " << moves[c2] << std::endl;

                int res = referee(c1, c2);
                if (res == 1) {
                    std::cout << "  Игрок " << msg1.player_id + 1 << " победил" << std::endl;
                    is_in_game[p2] = false;
                    round_winners[winners_count++] = p1;
                    decided = true;
                } else if (res == 2) {
                    std::cout << "  Игрок " << msg2.player_id + 1 << " победил" << std::endl;
                    is_in_game[p1] = false;
                    round_winners[winners_count++] = p2;
                    decided = true;
                } else {
                    std::cout << "  Ничья, матч переигрывается..." << std::endl;
                    sem_post_sysv(SEM_PLAYER_START + p1);
                    sem_post_sysv(SEM_PLAYER_START + p2);
                }
            }
        }

        activeStudents.clear();
        for (int i = 0; i < winners_count; i++) {
            activeStudents.push_back(round_winners[i]);
        }

        if (activeStudents.size() == 1) {
            tournament_winner = activeStudents[0] + 1;
            std::cout << "\nТурнир закончен, выиграл игрок под номером: " << tournament_winner << std::endl;
            break;
        }
    }

    cleanup();
    return 0;
} 