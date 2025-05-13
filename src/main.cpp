#include <boost/asio.hpp>
#include <iostream>
#include <set>
#include <memory>
#include <string>
#include <deque>
#include <functional>

using boost::asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, std::set<std::shared_ptr<Session>>& sessions)
        : socket_(std::move(socket)),
        strand_(socket_.get_executor()),
        sessions_(sessions)
    {
        // Disable Nagle for this socket
        boost::asio::ip::tcp::no_delay option(true);
        socket_.set_option(option);
    }

    void start() {
        sessions_.insert(shared_from_this());
        do_read();
    }

    // 외부에서 호출하는 브로드캐스트 진입점
    void deliver(const std::string& msg) {
        // 모든 deliver 호출을 strand 안에서 순차 실행
        boost::asio::post(strand_,
            [self = shared_from_this(), msg]() {
                bool write_in_progress = !self->write_msgs_.empty();
                self->write_msgs_.push_back(msg);
                if (!write_in_progress)
                    self->do_write();
            });
    }

private:
    void do_read() {
        auto self = shared_from_this();
        boost::asio::async_read_until(socket_, buffer_, '\n',
            boost::asio::bind_executor(strand_,
                [this, self](boost::system::error_code ec, std::size_t length) {
                    if (ec) {
                        sessions_.erase(self);
                        return;
                    }

                    // 1) 한 줄 읽기 (개행 제외)
                    std::istream is(&buffer_);
                    std::string received;
                    std::getline(is, received);
                    if (!received.empty() && received.back() == '\r')
                        received.pop_back();

                    // 2) 버퍼에서 해당 바이트 제거
                    buffer_.consume(length);

                    // 3) 인증 단계
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

                    // 4) 송신 포맷 준비
                    std::string outbound = "MSG|" + display_name_ + "|" + received + "\n";

                    // 5) 다른 세션에 전달
                    for (auto& s : sessions_) {
                        if (s != self && s->authenticated_) {
                            s->deliver(outbound);
                        }
                    }

                    // 6) 다시 읽기 대기
                    do_read();
                }));
    }

    void do_write() {
        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(write_msgs_.front()),
            boost::asio::bind_executor(strand_,
                [this, self](boost::system::error_code ec, std::size_t /*bytes*/) {
                    if (!ec) {
                        write_msgs_.pop_front();
                        if (!write_msgs_.empty())
                            do_write();
                    }
                    else {
                        sessions_.erase(self);
                    }
                }));
    }

    tcp::socket                                socket_;
    boost::asio::strand<tcp::socket::executor_type> strand_;
    boost::asio::streambuf                     buffer_;
    std::set<std::shared_ptr<Session>>& sessions_;
    std::deque<std::string>                    write_msgs_;
    std::string                                display_name_;
    bool                                       authenticated_ = false;
};

int main() {
    try {
        boost::asio::io_context io_context;

        tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), 12345));
        std::set<std::shared_ptr<Session>> sessions;

        std::function<void()> do_accept = [&]() {
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
    catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }
    return 0;
}


