#include <iostream>
#include <vector>
#include <random>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <cstring>
#include <cmath>
#include <mqueue.h>
#include <semaphore.h>
#include <fcntl.h>

#define SEM_MAIN "/main_sem"
#define SEM_PLAYER "/player_"

// структура для передачи сообщений между процессами
struct MoveMsg {
    int player_id;
    int move;
};

sem_t *sem_main;
std::vector<sem_t*> sem_player_start;
std::vector<std::string> sem_names;
mqd_t mqd;
std::string mq_name;
std::vector<pid_t> playerPids;
int n;
std::string moves[3] = {"камень", "ножницы", "бумага"};

void cleanup() {
    for (pid_t pid : playerPids) {
        kill(pid, SIGTERM);
    }

    for (pid_t pid : playerPids) {
        waitpid(pid, nullptr, 0);
    }

    sem_close(sem_main);
    sem_unlink(SEM_MAIN);

    for (int i = 0; i < n; i++) {
        sem_close(sem_player_start[i]);
        sem_unlink(sem_names[i].c_str());
    }

    mq_close(mqd);
    mq_unlink(mq_name.c_str());

    exit(0);
}

void handle_sigint(int sig) {
    std::cout << "Получен SIGINT, завершаю турнир и очищаю ресурсы..." << std::endl;
    cleanup();
}

void playerProcess(int id) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, 2);

    while (true) {
        sem_wait(sem_player_start[id - 1]);
        sem_wait(sem_main);

        int move = distrib(gen);
        MoveMsg msg = { 
            id - 1,
            move 
        };
        if (mq_send(mqd, reinterpret_cast<const char*>(&msg), sizeof(msg), 0) == -1) {
            perror("mq_send");
            exit(1);
        }

        sem_post(sem_main);
    }
    exit(0);
}

int referee(int move1, int move2) {
    if (move1 == move2) {
        return 0;
    } else if (move1 == 0 && move2 == 1 || move1 == 1 && move2 == 2 || move1 == 2 && move2 == 0) {
        return 1;
    }
    return 2;
};


int main() {
    struct sigaction sa{};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, nullptr);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distribPlayers(2, 101);
    n = distribPlayers(gen);
    std::cout << "Количество игроков в турнире: " << n << std::endl;

    mq_name = "/mq_" + std::to_string(getpid());
    struct mq_attr attr{};
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(MoveMsg);
    attr.mq_curmsgs = 0;

    mqd = mq_open(mq_name.c_str(), O_CREAT | O_RDWR, 0666, &attr);
    if (mqd == (mqd_t)-1) {
        perror("mq_open");
        return 1;
    }

    sem_main = sem_open(SEM_MAIN, O_CREAT | O_EXCL, 0666, 1);
    if (sem_main == SEM_FAILED) { 
        perror("sem_open sem_main");
        cleanup();
    }


    sem_player_start.resize(n);
    sem_names.resize(n);
    for (int i = 0; i < n; i++) {
        sem_names[i] = SEM_PLAYER + std::to_string(i);
        sem_player_start[i] = sem_open(sem_names[i].c_str(), O_CREAT | O_EXCL, 0666, 0);
        if (sem_player_start[i] == SEM_FAILED) {
            perror("sem_open sem_player_start");
            cleanup(); 
        }
    }

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            cleanup();
        }

        if (pid == 0) {
            // устанавливаю обработчик сигнала SIGINT по умолчанию,
            // чтобы не происходило очистки ресурсов при SIGINT в дочерних процессах
            signal(SIGINT, SIG_DFL);
            playerProcess(i + 1);
        }
        playerPids.push_back(pid);
    }

    std::vector<bool> is_in_game(n, true);
    std::vector<int> round_winners(n);
    int winners_count = 0;
    int tournament_winner = -1;

    int numRounds = static_cast<int>(std::ceil(std::log2(n)));
    std::vector<int> activeStudents(n);
    for (int i = 0; i < n; i++) {
        activeStudents[i] = i;
    }

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

            sem_post(sem_player_start[p1]);
            sem_post(sem_player_start[p2]);

            bool decided = false;
            while (!decided) {
                MoveMsg msg1, msg2;
                if (mq_receive(mqd, reinterpret_cast<char*>(&msg1), sizeof(msg1), nullptr) == -1) {
                    perror("mq_receive");
                    cleanup();
                }

                if (mq_receive(mqd, reinterpret_cast<char*>(&msg2), sizeof(msg2), nullptr) == -1) {
                    perror("mq_receive");
                    cleanup();
                }

                int c1 = msg1.move;
                int c2 = msg2.move;

                std::cout << "   Игрок " << msg1.player_id + 1 << " выбрал " << moves[c1] << std::endl;
                std::cout << "   Игрок " << msg2.player_id + 1 << " выбрал " << moves[c2] << std::endl;

                int res = referee(c1, c2);
                if (res == 1) {
                    std::cout << "  Игрок " << msg1.player_id + 1 << " победил" << std::endl;
                    round_winners[winners_count++] = p1;
                    decided = true;
                } else if (res == 2) {
                    std::cout << "  Игрок " << msg2.player_id + 1 << " победил" << std::endl;
                    round_winners[winners_count++] = p2;
                    decided = true;
                } else {
                    std::cout << "  Ничья, матч переигрывается..." << std::endl;
                    sem_post(sem_player_start[p1]);
                    sem_post(sem_player_start[p2]);
                }
            }
        }

        activeStudents.clear();
        for (int i = 0; i < winners_count; i++) activeStudents.push_back(round_winners[i]);
        if (activeStudents.size() == 1) {
            tournament_winner = activeStudents[0] + 1;
            std::cout << "\nТурнир закончен, выиграл игрок под номером: " << tournament_winner << std::endl;
            break;
        }
    }

    cleanup();
    return 0;
} 