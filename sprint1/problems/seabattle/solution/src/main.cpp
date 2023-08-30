#ifdef WIN32
#include <sdkddkver.h>
#endif

#include "seabattle.h"

#include <atomic>
#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <string_view>
// #include <time.h>


namespace net = boost::asio;
using net::ip::tcp;
using namespace std::literals;

void PrintFieldPair(const SeabattleField& left, const SeabattleField& right) {
    auto left_pad = "  "s;
    auto delimeter = "    "s;
    std::cout << left_pad;
    SeabattleField::PrintDigitLine(std::cout);
    std::cout << delimeter;
    SeabattleField::PrintDigitLine(std::cout);
    std::cout << std::endl;
    for (size_t i = 0; i < SeabattleField::field_size; ++i) {
        std::cout << left_pad;
        left.PrintLine(std::cout, i);
        std::cout << delimeter;
        right.PrintLine(std::cout, i);
        std::cout << std::endl;
    }
    std::cout << left_pad;
    SeabattleField::PrintDigitLine(std::cout);
    std::cout << delimeter;
    SeabattleField::PrintDigitLine(std::cout);
    std::cout << std::endl;
}

template <size_t sz>
static std::optional<std::string> ReadExact(tcp::socket& socket) {
    boost::array<char, sz> buf;
    boost::system::error_code ec;

    net::read(socket, net::buffer(buf), net::transfer_exactly(sz), ec);

    if (ec) {
        return std::nullopt;
    }

    return {{buf.data(), sz}};
}

static bool WriteExact(tcp::socket& socket, std::string_view data) {
    boost::system::error_code ec;

    net::write(socket, net::buffer(data), net::transfer_exactly(data.size()), ec);

    return !ec;
}

static void ApplyShotToField(const SeabattleField::ShotResult& shotResult, const std::pair<int, int>& _move, SeabattleField& field) {
    switch (shotResult) {
    case SeabattleField::ShotResult::HIT:
        field.MarkHit(_move.first, _move.second);
        break;
    case SeabattleField::ShotResult::KILL:
        field.MarkKill(_move.first, _move.second);
        break;
    case SeabattleField::ShotResult::MISS:
        field.MarkMiss(_move.first, _move.second);
        break;
    default:
        std::cout << "Error in applying shot to field"sv << std::endl;
    }
}

static std::optional<std::string> ShotResultToString(const SeabattleField::ShotResult& shotResult) {
    switch (shotResult) {
    case SeabattleField::ShotResult::HIT:
        return "HIT";
    case SeabattleField::ShotResult::KILL:
        return "KILL";
    case SeabattleField::ShotResult::MISS:
        return "MISS";
    default:
        return std::nullopt;
    }
}

class SeabattleAgent {
public:
    SeabattleAgent(const SeabattleField& field)
        : my_field_(field) {
    }

    void StartGame(tcp::socket& socket, bool my_initiative) {
        //
        PrintFields();
        while ( !IsGameEnded() ) { // пока кто-то не выиграл
            if (my_initiative) { // наш ход
                std::cout << "Your turn: "sv;
                std::string sYourMove;
                std::cin >> sYourMove;
                auto yourMove = ParseMove(sYourMove);

                if (yourMove != std::nullopt) {
                    SendMove(socket, yourMove.value()); // посылаем ход через сокет

                    auto str_shotResult = ReadResult(socket); // считываем из сокета результат выстрела
                    if (str_shotResult == std::nullopt) {
                        std::cout << "Error reading shot result"sv << std::endl;
                        continue;
                    }

                    auto shotResult = static_cast<SeabattleField::ShotResult>(str_shotResult.value().c_str()[0]); // из строки берем первый символ и конвертируем его в SeabattleField::ShotResult

                    auto str_msg_shotResult = ShotResultToString(shotResult);
                    if (str_msg_shotResult == std::nullopt) {
                        std::cout << "Error converting shot result"sv << std::endl;
                        continue;
                    }
                    std::cout << "Shot result: "sv << str_msg_shotResult.value() << std::endl;

                    ApplyShotToField(shotResult, yourMove.value(), other_field_); // применяем ход и результат к карте противника

                    PrintFields();

                    // меняем инициативу, если мы промахнулись
                    if (shotResult == SeabattleField::ShotResult::MISS) {
                        my_initiative = !my_initiative;
                    }

                }
                else { // если ход был написан неправильно
                    std::cout << "Wrong move format."sv << std::endl
                        << "Please write move as 2 symbols - first from A to H, second from 1 to 8"sv << std::endl;
                    continue;
                }
                
            }
            else { // ход соперника
                std::cout << "Waiting for turn..."sv << std::endl;

                auto str_move = ReadMove(socket);
                if (str_move == std::nullopt) {
                    std::cout << "Error reading move"sv << std::endl;
                    continue;
                }

                std::cout << "Shot to "sv << str_move.value() << std::endl;

                auto _move = ParseMove(str_move.value());
                if (_move == std::nullopt) {
                    std::cout << "Error parsing move"sv << std::endl;
                    continue;
                }

                auto shotResult = my_field_.Shoot(_move->first, _move->second); 
                ApplyShotToField(shotResult, _move.value(), my_field_); // применяем полученный ход к своей карте

                PrintFields();

                SendResult(socket, shotResult); // отправляем результат выстрела

                // меняем инициативу, если противник промахнулся
                if (shotResult == SeabattleField::ShotResult::MISS) {
                    my_initiative = !my_initiative;
                }
            }
        }
        std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
        if (other_field_.IsLoser()) {
            std::cout << "Congratulations, you won!"sv << std::endl;
        } 
        else if (my_field_.IsLoser()) {
            std::cout << "You lose :( "sv << std::endl;
        }
        else {
            std::cout << "Something strange hapenned with game end !"sv << std::endl;
        }
    }

private:
    static std::optional<std::pair<int, int>> ParseMove(const std::string_view& sv) {
        if (sv.size() != 2) return std::nullopt;

        int p1 = sv[0] - 'A', p2 = sv[1] - '1';

        if (p1 < 0 || p1 > 8) return std::nullopt;
        if (p2 < 0 || p2 > 8) return std::nullopt;

        return {{p2, p1}};
    }

    static std::string MoveToString(std::pair<int, int> move) {
        char buff[] = {static_cast<char>(move.second) + 'A', static_cast<char>(move.first) + '1'};
        return {buff, 2};
    }

    void PrintFields() const {
        PrintFieldPair(my_field_, other_field_);
    }

    bool IsGameEnded() const {
        return my_field_.IsLoser() || other_field_.IsLoser();
    }

    //
    static void SendMove(tcp::socket& socket, const std::pair<int, int>& _move) {
        std::string str = MoveToString(_move);
        if ( !WriteExact(socket, str) ) {
            std::cout << "Error sending move"sv << std::endl;
            return;
        }
    }

    static std::optional<std::string> ReadMove(tcp::socket& socket) {
        return ReadExact<2>(socket);
    }

    static void SendResult(tcp::socket& socket, const SeabattleField::ShotResult& shotResult) {
        char sentResult = static_cast<char>(shotResult);
        if ( !WriteExact(socket, std::string_view(&sentResult, 1)) ) {
            std::cout << "Error sending result"sv << std::endl;
            return;
        }
    }

    static std::optional<std::string> ReadResult(tcp::socket& socket) {
        return ReadExact<1>(socket);
    }

private:
    SeabattleField my_field_;
    SeabattleField other_field_;
};

void StartServer(const SeabattleField& field, unsigned short port) {
    SeabattleAgent agent(field);

    //
    net::io_context io_context;

    tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), port));
    std::cout << "Waiting for connection..."sv << std::endl;

    boost::system::error_code err_code;
    tcp::socket socket{ io_context };
    acceptor.accept(socket, err_code);

    if (err_code) {
        std::cout << "Can't accept connection"sv << std::endl;
        return;
    }

    agent.StartGame(socket, false);
};

void StartClient(const SeabattleField& field, const std::string& ip_str, unsigned short port) {
    SeabattleAgent agent(field);

    //
    boost::system::error_code err_code;
    auto endpoint = tcp::endpoint(net::ip::make_address(ip_str, err_code), port);

    if (err_code) {
        std::cout << "Wrong IP format"sv << std::endl;
        return;
    }

    net::io_context io_context;
    tcp::socket socket{ io_context };
    socket.connect(endpoint, err_code);

    if (err_code) {
        std::cout << "Can't connect to server"sv << std::endl;
        return;
    }

    agent.StartGame(socket, true);
};

int main(int argc, const char** argv) {
    if (argc != 3 && argc != 4) {
        std::cout << "Usage: program <seed> [<ip>] <port>" << std::endl;
        return 1;
    }

    /*int min = 0;
    int max = 9999;
    srand(time(0));
    int pseudo_rand_seed = ((double)rand() / (RAND_MAX + 1)) * (max - min + 1) + min;
    std::mt19937 engine(pseudo_rand_seed);*/
    std::mt19937 engine(std::stoi(argv[1]));
    SeabattleField fieldL = SeabattleField::GetRandomField(engine);

    if (argc == 3) {
        StartServer(fieldL, std::stoi(argv[2]));
    } else if (argc == 4) {
        StartClient(fieldL, argv[2], std::stoi(argv[3]));
    }
    system("pause");
}
