#include <iostream>
#include <vector>
#include <random>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <cstring>
#include <cmath>
#include <signal.h>
#include <string>

// union для семафоров
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

// номера семафоров для удобства
#define SEM_MAIN 0 
#define SEM_MOVE 1
#define SEM_PLAYER_START 2 

struct TournamentState {
    int players_moves[100];
    bool is_in_game[100];
    int opponent[100];
    int tournament_winner;
    int round_winners[100];
    int winners_count;
    int current_round;
    int total_players;
    bool is_finished;
};

int semid = -1;
int shmid = -1;
TournamentState *tournamentState;
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
    if (move1 == move2) return 0;
    if ((move1 == 0 && move2 == 1) || (move1 == 1 && move2 == 2) || (move1 == 2 && move2 == 0))
        return 1;
    return 2;
}

void playerProcess(int id) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, 2);

    while (true) {
        sem_wait_sysv(SEM_PLAYER_START + id - 1);
        sem_wait_sysv(SEM_MAIN);

        if (tournamentState->is_finished || !tournamentState->is_in_game[id - 1]) {
            sem_post_sysv(SEM_MAIN);
            break;
        }

        int move = distrib(gen);
        tournamentState->players_moves[id - 1] = move;

        sem_post_sysv(SEM_MAIN);
        sem_post_sysv(SEM_MOVE);
    }
    exit(0);
}

void handle_sigint(int sig) {
    std::cout << "Получен SIGINT, завершаю турнир и очищаю ресурсы..." << std::endl;
    for (int i = 0; i < n; i++) {
        sem_post_sysv(SEM_PLAYER_START + i);
    }

    for (pid_t pid : playerPids) {
        waitpid(pid, nullptr, 0);
    }

    if (semid != -1) {
        if (semctl(semid, 0, IPC_RMID) == -1) {
            perror("semctl IPC_RMID");
        }
    }

    if (tournamentState) {
        shmdt(tournamentState);
    }

    if (shmid != -1) {
        shmctl(shmid, IPC_RMID, nullptr);
    }

    exit(0);
}

int main() {
    struct sigaction sa{};
    sa.sa_handler = handle_sigint;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(2, 101);
    n = distrib(gen);
    std::cout << "Количество игроков в турнире: " << n << std::endl;

    shmid = shmget(IPC_PRIVATE, sizeof(TournamentState), IPC_CREAT | 0666);

    if (shmid < 0) {
        perror("shmget"); 
        return 1; 
    }

    tournamentState = static_cast<TournamentState*>(shmat(shmid, nullptr, 0));

    if (tournamentState == reinterpret_cast<TournamentState*>(-1)) {
        perror("shmat"); 
        return 1; 
    }

    memset(tournamentState, 0, sizeof(TournamentState));
   
    int totalSems = SEM_PLAYER_START + n;
    semid = semget(IPC_PRIVATE, totalSems, IPC_CREAT | 0666);
    if (semid < 0) {
        perror("semget");
        return 1;
    }

    union semun arg;

    arg.val = 1;
    if (semctl(semid, SEM_MAIN, SETVAL, arg) == -1) {
        perror("semctl main");
        return 1;
    }
    
    arg.val = 0;
    if (semctl(semid, SEM_MOVE, SETVAL, arg) == -1) {
        perror("semctl move");
        return 1;
    }

    for (int i = 0; i < n; i++) {
        arg.val = 0;
        if (semctl(semid, SEM_PLAYER_START + i, SETVAL, arg) == -1) {
            perror("semctl player");
            return 1;
        }
    }

    tournamentState->total_players = n;
    tournamentState->tournament_winner = -1;
    tournamentState->is_finished = false;
    tournamentState->current_round = 1;

    for (int i = 0; i < n; i++) {
        tournamentState->is_in_game[i] = true;
        tournamentState->opponent[i] = -1;
    }

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();

        if (pid < 0) { 
            perror("fork"); 
            return 1; 
        }

        if (pid == 0) {
            playerProcess(i + 1);
        }
        
        playerPids.push_back(pid);
    }

    int numRounds = static_cast<int>(std::ceil(std::log2(n)));

    std::vector<int> activeStudents(n);
    for (int i = 0; i < n; i++) {
        activeStudents[i] = i;
    }

    for (int round = 1; round <= numRounds; round++) {
        std::cout << "\nРаунд " << round << std::endl;
        tournamentState->winners_count = 0;

        sem_wait_sysv(SEM_MAIN);
        tournamentState->current_round = round;
        sem_post_sysv(SEM_MAIN);

        int matchCount = activeStudents.size() / 2;

        if (activeStudents.size() % 2 == 1) {
            int idx = activeStudents.back();
            std::cout << "Студент " << idx + 1 << " проходит в следующий раунд" << std::endl;

            sem_wait_sysv(SEM_MAIN);
            tournamentState->round_winners[tournamentState->winners_count++] = idx;
            sem_post_sysv(SEM_MAIN);
        }

        for (int m = 0; m < matchCount; m++) {
            int p1 = activeStudents[2 * m];
            int p2 = activeStudents[2 * m + 1];
            std::cout << "Матч между " << p1 + 1 << " и " << p2 + 1 << std::endl;

            sem_wait_sysv(SEM_MAIN);
            tournamentState->opponent[p1] = p2;
            tournamentState->opponent[p2] = p1;
            sem_post_sysv(SEM_MAIN);

            sem_post_sysv(SEM_PLAYER_START + p1);
            sem_post_sysv(SEM_PLAYER_START + p2);

            bool decided = false;
            while (!decided) {
                sem_wait_sysv(SEM_MOVE);
                sem_wait_sysv(SEM_MOVE);

                sem_wait_sysv(SEM_MAIN);
                int c1 = tournamentState->players_moves[p1];
                int c2 = tournamentState->players_moves[p2];
                sem_post_sysv(SEM_MAIN);

                std::cout << "   Игрок " << p1 + 1 << " выбрал " << moves[c1] << std::endl;
                std::cout << "   Игрок " << p2 + 1 << " выбрал " << moves[c2] << std::endl;

                int res = referee(c1, c2);
                
                if (res == 1) {
                    std::cout << "  Игрок " << p1+1 << " победил" << std::endl;
                    sem_wait_sysv(SEM_MAIN);
                    tournamentState->is_in_game[p2] = false;
                    tournamentState->round_winners[tournamentState->winners_count++] = p1;
                    sem_post_sysv(SEM_MAIN);
                    decided = true;
                } else if (res == 2) {
                    std::cout << "  Игрок " << p2+1 << " победил" << std::endl;
                    sem_wait_sysv(SEM_MAIN);
                    tournamentState->is_in_game[p1] = false;
                    tournamentState->round_winners[tournamentState->winners_count++] = p2;
                    sem_post_sysv(SEM_MAIN);
                    decided = true;
                } else {
                    std::cout << "  Ничья, матч переигрывается..." << std::endl;
                    sem_post_sysv(SEM_PLAYER_START + p1);
                    sem_post_sysv(SEM_PLAYER_START + p2);
                }
            }
        }

        activeStudents.clear();

        sem_wait_sysv(SEM_MAIN);
        for (int i = 0; i < tournamentState->winners_count; i++) {
            activeStudents.push_back(tournamentState->round_winners[i]);
        }
        sem_post_sysv(SEM_MAIN);

        if (activeStudents.size() == 1) {
            sem_wait_sysv(SEM_MAIN);
            tournamentState->tournament_winner = activeStudents[0] + 1;
            tournamentState->is_finished = true;
            sem_post_sysv(SEM_MAIN);

            std::cout << "\nТурнир закончен, выиграл игрок под номером: " << tournamentState->tournament_winner << std::endl;
            break;
        }
    }

    for (int i = 0; i < n; i++) {
        sem_post_sysv(SEM_PLAYER_START + i);
    }

    for (pid_t pid : playerPids) {
        if (waitpid(pid, nullptr, 0) == -1) {
            perror("waitpid");
        }
    }

    if (semid != -1) {
        if (semctl(semid, 0, IPC_RMID) == -1) {
            perror("semctl IPC_RMID");
        }
    }
    if (tournamentState) {
        if (shmdt(tournamentState) == -1) {
            perror("shmdt");
        }
    }
    if (shmid != -1) {
        if (shmctl(shmid, IPC_RMID, nullptr) == -1) {
            perror("shmctl IPC_RMID");
        }
    }
    return 0;
} 