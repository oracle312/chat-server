//

#include <boost/asio.hpp>
#include <pqxx/pqxx>
#include <iostream>
#include <set>
#include <memory>
#include <string>
#include <deque>
#include <functional>
#include <unordered_map>

using boost::asio::ip::tcp;

class Session;
static std::unordered_map<std::string, std::set<std::shared_ptr<Session>>> rooms;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, pqxx::connection& db, std::set<std::shared_ptr<Session>>& sessions)
        : socket_(std::move(socket)), strand_(socket_.get_executor()), db_(db), sessions_(sessions) {
        socket_.set_option(tcp::no_delay(true));
    }

    void start() {
        sessions_.insert(shared_from_this());
        do_read();
    }

    void deliver(const std::string& msg) {
        boost::asio::post(strand_, [self = shared_from_this(), msg]() {
            bool write_in_progress = !self->write_msgs_.empty();
            self->write_msgs_.push_back(msg);
            if (!write_in_progress)
                self->do_write();
            });
    }

private:
    void send_user_list() {
        std::string list = "USERS|";
        bool first = true;
        for (auto& s : sessions_) {
            if (s->authenticated_) {
                if (!first) list += ",";
                list += s->display_name_;
                first = false;
            }
        }
        list += "\n";
        for (auto& s : sessions_) {
            if (s->authenticated_)
                s->deliver(list);
        }
    }

    void do_read() {
        auto self = shared_from_this();
        boost::asio::async_read_until(socket_, buffer_, '\n', boost::asio::bind_executor(strand_,
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (ec) {
                    sessions_.erase(self);
                    for (auto& room : joined_rooms_)
                        rooms[room].erase(self);
                    send_user_list();
                    return;
                }

                std::istream is(&buffer_);
                std::string received;
                std::getline(is, received);
                if (!received.empty() && received.back() == '\r')
                    received.pop_back();
                buffer_.consume(length);

                if (!authenticated_) {
                    if (received.rfind("AUTH|", 0) == 0) {
                        display_name_ = received.substr(5);
                        authenticated_ = true;
                        send_user_list();
                    }
                    do_read();
                    return;
                }

                if (received.rfind("JOIN|", 0) == 0) {
                    std::string room = received.substr(5);
                    rooms[room].insert(self);
                    joined_rooms_.insert(room);

                    try {
                        pqxx::work txn(db_);
                        pqxx::result r = txn.exec_params("SELECT sender, message FROM chat_messages WHERE room_id=$1 ORDER BY created_at ASC LIMIT 50", room);
                        for (const auto& row : r) {
                            std::string sender = row[0].as<std::string>();
                            std::string message = row[1].as<std::string>();
                            deliver("MSG|" + room + "|" + sender + "|" + message + "\n");
                        }
                    }
                    catch (const std::exception& e) {
                        std::cerr << "[DB READ ERROR] " << e.what() << std::endl;
                    }
                }
                else if (received.rfind("MSG|", 0) == 0) {
                    auto pos1 = received.find('|');
                    auto pos2 = received.find('|', pos1 + 1);
                    std::string room = received.substr(pos1 + 1, pos2 - pos1 - 1);
                    std::string msg = received.substr(pos2 + 1);

                    try {
                        pqxx::work txn(db_);
                        txn.exec_params("INSERT INTO chat_messages (room_id, sender, message) VALUES ($1, $2, $3)", room, display_name_, msg);
                        txn.commit();
                    }
                    catch (const std::exception& e) {
                        std::cerr << "[DB WRITE ERROR] " << e.what() << std::endl;
                    }

                    std::string outbound = "MSG|" + room + "|" + display_name_ + "|" + msg + "\n";

                    // 여기에서 상대방이 room에 JOIN하지 않은 경우도 강제 참여
                    for (auto& s : sessions_) {
                        if (s->authenticated_) {
                            if (rooms[room].find(s) == rooms[room].end()) {
                                rooms[room].insert(s);
                                s->joined_rooms_.insert(room);
                            }
                            s->deliver(outbound);
                        }
                    }
                }

                do_read();
            }));
    }

    void do_write() {
        auto self = shared_from_this();
        boost::asio::async_write(socket_, boost::asio::buffer(write_msgs_.front()),
            boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    write_msgs_.pop_front();
                    if (!write_msgs_.empty())
                        do_write();
                }
                else {
                    sessions_.erase(self);
                    for (auto& room : joined_rooms_)
                        rooms[room].erase(self);
                    send_user_list();
                }
                }));
    }

    tcp::socket socket_;
    boost::asio::strand<tcp::socket::executor_type> strand_;
    boost::asio::streambuf buffer_;
    std::set<std::shared_ptr<Session>>& sessions_;
    std::deque<std::string> write_msgs_;
    std::set<std::string> joined_rooms_;
    pqxx::connection& db_;
    std::string display_name_;
    bool authenticated_ = false;
};

int main() {
    try {
        pqxx::connection db("dbname=chatdb user=chatuser password=dio0660 host=localhost");
        boost::asio::io_context io_context;
        tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), 12345));
        std::set<std::shared_ptr<Session>> sessions;

        std::function<void()> do_accept;
        do_accept = [&]() {
            acceptor.async_accept([&](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket), db, sessions)->start();
                }
                do_accept();
                });
            };

        do_accept();
        std::cout << "Server running on port 12345..." << std::endl;
        io_context.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }
    return 0;
}




