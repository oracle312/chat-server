#include <boost/asio.hpp>
#include <iostream>
#include <set>
#include <memory>

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
        socket_.async_read_some(boost::asio::buffer(data_, max_length),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    // 브로드캐스트
                    for (auto& s : sessions_) {
                        if (s != self) {
                            s->socket_.async_write_some(
                                boost::asio::buffer(data_, length),
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
        std::cout << "Server running on port 12345...\n";
        io_context.run();
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }
    return 0;
}
