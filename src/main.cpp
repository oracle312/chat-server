#include <boost/asio.hpp>
#include <iostream>
#include <set>
#include <memory>
#include <string>

using boost::asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, std::set<std::shared_ptr<Session>>& sessions)
        : socket_(std::move(socket)), sessions_(sessions) {
    }

    void start() {
        sessions_.insert(shared_from_this());
        do_read();
    }

private:
    void do_read() {
        auto self(shared_from_this());
        boost::asio::async_read_until(socket_, buffer_, '\n',
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    // 1) 한 줄(개행 포함) 읽기
                    std::istream is(&buffer_);
                    std::string received;
                    std::getline(is, received);  // '\n' 제거된 메시지가 received에 담깁니다

                    if (!authenticated_) {
                        const std::string prefix = "AUTH|";
                        if (received.rfind(prefix, 0) == 0) {
                            display_name_ = received.substr(prefix.size());
                            authenticated_ = true;
                            std::cout << "[Authenticated] " << display_name_ << std::endl;
                        }
                        do_read();
                        return;
                    }

                    // 2) 형식 맞춰서 한 줄로 브로드캐스트
                    std::string outbound = "MSG|" + display_name_ + "|" + received + "\n";

                    for (auto& s : sessions_) {
                        if (s != self && s->authenticated_) {
                            // async_write 로 전체를 보장
                            boost::asio::async_write(
                                s->socket_,
                                boost::asio::buffer(outbound),
                                [](auto, auto) {}
                            );
                        }
                    }
                    do_read();
                }
                else {
                    sessions_.erase(self);
                }
            });
    }

    tcp::socket socket_;
    enum { max_length = 1024 };
    char data_[max_length];
    std::set<std::shared_ptr<Session>>& sessions_;
    std::string display_name_;
    bool authenticated_ = false;
};

int main() {
    try {
        boost::asio::io_context io_context;
        tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), 12345));
        std::set<std::shared_ptr<Session>> sessions;

        std::function<void()> do_accept;
        do_accept = [&]() {
            acceptor.async_accept(
                [&](boost::system::error_code ec, tcp::socket socket) {
                    if (!ec) {
                        std::make_shared<Session>(std::move(socket), sessions)->start();
                    }
                    do_accept();
                });
            };

        do_accept();
        std::cout << "Server running on port 12345..." << std::endl;
        io_context.run();
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }
    return 0;
}

