#include <amqpcpp.h>
#include <amqpcpp/libevent.h>
#include <event2/event.h>

#include <iostream>
#include <string>
#include <thread>
#include <iostream>
#include <functional>
#include <unistd.h>
#include <memory>
#include <atomic>

class LibEventHandlerMyError : public AMQP::LibEventHandler
{
public:
	LibEventHandlerMyError(struct event_base* evbase) : LibEventHandler(evbase), evbase_(evbase) 
	{
		isConnectionReady_ = false;
	}
	virtual ~LibEventHandlerMyError() = default;

	void onError(AMQP::TcpConnection *connection, const char *message) override
	{
		std::cout << "LibEventHandler - Error: " << message << std::endl;
		setConnectionReady(false);
	}

	void onHeartbeat(AMQP::TcpConnection *connection) override
	{
		std::cout << "LibEventHandler - Heartbeat " << std::endl;
        connection->heartbeat();
    }

    void onReady(AMQP::TcpConnection *connection) override 
    {
        std::cout << "LibEventHandler - TCP connection is ready." << std::endl;
    }

    void onClosed(AMQP::TcpConnection *connection) override 
    {
        std::cout << "LibEventHandler - TCP connection has been closed." << std::endl;
		setConnectionReady(false);
    }

    void onDetached(AMQP::TcpConnection *connection) override 
    {
        std::cout << "LibEventHandler - TCP connection has been detached." << std::endl;
		setConnectionReady(false);
    }

	bool isConnectionReady()
	{
		return isConnectionReady_.load();
	}

	void setConnectionReady(bool isReady)
	{
		std::cout << "LibEventHandler - Connection is ready: " << isReady << std::endl;
		isConnectionReady_ = isReady;
	}
	
private:
	struct event_base* evbase_ {nullptr};
	std::atomic<bool> isConnectionReady_;
};


class ConnHandler
{
public:
	using EventBasePtrT = std::unique_ptr<struct event_base, std::function<void(struct event_base*)> >;
	using EventPtrT = std::unique_ptr<struct event, std::function<void(struct event*)> >;

	ConnHandler()
		: evbase_(event_base_new(), event_base_free)
		  , evhandler_(evbase_.get())
	{
	}

	void Start()
	{
		event_base_dispatch(evbase_.get());
	}
	void Stop()
	{
		event_base_loopbreak(evbase_.get());
	}

	operator AMQP::TcpHandler*()
	{
		return &evhandler_;
	}

	void setConnectionReady(bool isReady)
	{
		evhandler_.setConnectionReady(isReady);
	}
	bool isConnectionReady()
	{
		return evhandler_.isConnectionReady();
	}

    event_base* getEventBase()
    {
        return evbase_.get();
    }

private:

	EventBasePtrT evbase_;
	LibEventHandlerMyError evhandler_;
};



class RabbitMQTransmitter
{
public:
    RabbitMQTransmitter(const std::string& exchangeName, const std::string& queueName)
        : exchangeName_(exchangeName), queueName_(queueName)
    {
        // Initialize libevent - not sure if this is needed!!!
        // event_config* ev_config = event_config_new();
        // event_config_set_flag(ev_config, EVENT_BASE_FLAG_EPOLL_USE_CHANGELIST);
        // evbase_ = event_base_new_with_config(ev_config);
        // event_config_free(ev_config);
        
        event_loop_thread_ = std::thread(&RabbitMQTransmitter::EventLoop, this);
    }
    ~RabbitMQTransmitter()
    {
        Stop();
    }

    void EventLoop() //processing
    {       
        while(true)
        {
            Start();
            
            // Start event loop
            conn_handler_->Start();
          
            // Clean up
            //event_base_dispatch(evbase_);
            if (message_event_)
                event_del(message_event_);
            channel_.reset();
            connection_.reset();
        }
    }

    void Start()
    {
        conn_handler_ = std::make_shared<ConnHandler>();

        // Create a new connection and channel
        connection_ = std::make_shared<AMQP::TcpConnection>(*conn_handler_.get(), AMQP::Address("amqp://localhost/"));
        
        channel_ = std::make_shared<AMQP::TcpChannel>(connection_.get());
        
        channel_->onError([this](const char *message) {
            std::cout<<"(RMQ tx) Channel error: " << message;
            // Stop the handler
            conn_handler_->Stop();

        });

        // Declare the exchange and queue
        channel_->declareExchange(exchangeName_, AMQP::fanout)
                .onSuccess([this]() {
                    std::cout<<"(RMQ tx) Exchange '" << exchangeName_ << "' declared successfully.";
                    }
                )		
                .onError([this](const char *message) {
                    std::cout<<"(RMQ tx) Exchange '" << exchangeName_ << "' already exists. " << message;
                    }
                );

        channel_->declareQueue(queueName_, AMQP::durable)
                .onSuccess([this](const std::string& name, uint32_t, uint32_t) {
                queueName_ = name;
            
                channel_->bindQueue(exchangeName_, queueName_, "")
                        .onSuccess([this]() {
                            std::cout<<"(RMQ tx) Successfully bound queue to exchange.";
                            conn_handler_->setConnectionReady(true);
                            // Set up the message sending event
                            message_event_ =
                            event_new(conn_handler_->getEventBase(), -1, EV_PERSIST, &RabbitMQTransmitter::OnSendMessage, this);
            })
                .onError([](const char *message) {
                std::cout<<"(RMQ tx) Error binding queue to exchange: " << message;
            });
                
        });
    }

    void Stop()
    {
        // Disable the message sending event
        if (message_event_)
        {
            event_del(message_event_);
            message_event_ = nullptr;
        }
        // Stop the connection handler and event loop
        conn_handler_->Stop();
        if (event_loop_thread_.joinable())
        {
            event_loop_thread_.join();
        }
    }

    void SendMessage(const std::string& messageBody)
    {
        // Add the message to the message queue
        message_queue_.push(messageBody);
        if (message_event_ && conn_handler_->isConnectionReady())
        {
            // Activate the message sending event
            event_active(message_event_, EV_WRITE, 0);
        }
    }

private:
    static void OnSendMessage(evutil_socket_t fd, short events, void* arg)
    {
        auto self = reinterpret_cast<RabbitMQTransmitter*>(arg);
        if (events & EV_TIMEOUT)
        {
            std::cout << "OnSendMessage: timeout" << std::endl;
            return;
        }
        if (events & EV_WRITE)
        {
            while (!self->message_queue_.empty())
            {
                const std::string& messageBody = self->message_queue_.front();
                try
                {
                    AMQP::Envelope envelope(messageBody.c_str(), messageBody.size());
                    if (self->conn_handler_->isConnectionReady())
                    {
                        std::cout<<"publish!";
                        self->channel_->publish(self->exchangeName_, "", envelope);
                    }
                }
                catch (const std::exception& e)
                {
                    std::cerr << "Failed to send message: " << e.what() << std::endl;
                }
                self->message_queue_.pop();
            }
        }
    }

    std::string exchangeName_;
    std::string queueName_;
    std::shared_ptr<AMQP::TcpConnection> connection_;
    std::shared_ptr<AMQP::TcpChannel> channel_;
    std::shared_ptr<ConnHandler> conn_handler_;
    event* message_event_ = nullptr; //raw pointer for now
    std::queue<std::string> message_queue_;
    std::thread event_loop_thread_;
};


// We don't care about the receiver for now
// It has thread, but works fine, so for testing - let's leave it as it is
// This task is all about transmitter.
class RabbitMQReceiver 
{
public:
  RabbitMQReceiver(const std::string& exchangeName, const std::string& queueName, const std::string& routingKey)
      : exchangeName_(exchangeName), queueName_(queueName), routingKey_(routingKey) 
      {

        processingThread = std::thread(&RabbitMQReceiver::Start, this);

      }

    void Start() 
    {
        while(true)
        {    
            try 
            {
                connectionHandler_ = std::make_shared<ConnHandler>();

                connection_ = std::make_shared<AMQP::TcpConnection>(*connectionHandler_.get(), AMQP::Address("amqp://localhost/"));

                channel_ = std::make_shared<AMQP::TcpChannel>(connection_.get());

                channel_->onError([this](const char *message) {
                    std::cout<<"(RMQ rx) Channel error: " << message;

                    if (exchangeName_.empty())
                    {
                        std::cout<<"(RMQ rx) Details: exchange_name is empty!";
                    }
                    // Stop the handler
                    this->connectionHandler_->Stop();

                });
                channel_->declareExchange(exchangeName_, AMQP::fanout)
                    .onSuccess([this]() {
                        std::cout<<"(RMQ rx) Exchange '" << exchangeName_ << "' declared successfully.";
                        }
                    )		
                    .onError([this](const char *message) {
                        std::cout<<"(RMQ rx) Exchange '" << exchangeName_ << "' already exists. " << message;
                        }
                    );

                channel_->declareQueue(queueName_, AMQP::durable)
                    .onSuccess
                    (
                        [](const std::string &name,
                        uint32_t messagecount,
                        uint32_t consumercount) {
                            std::cout<<"(RMQ rx) Created queue: " << name;
                        }
                    )
                    .onError([](const char *message) {
                        std::cout<<"(RMQ rx) Channel queue Error: " << message;
                        }
                    );

                channel_->bindQueue(exchangeName_, queueName_, "")
                    .onSuccess([]() {
                        std::cout<<"(RMQ rx) Successfully bound queue to exchange.";
                    })
                    .onError([](const char *message) {
                        std::cout<<"(RMQ rx) Error binding queue to exchange: " << message;
                    });


                channel_->consume(queueName_)
                    .onReceived([this](const AMQP::Message& message, uint64_t tag, bool) {
                        auto messageBody = std::string(message.body(), message.bodySize());
                        std::cout<< "Message consumed: "<<messageBody;
                        channel_->ack(tag);
                    })
                    .onError([](const char* message) {
                        std::cerr << "Received an error while consuming message: " << message << std::endl;
                    });
                    
            } 
            catch (const std::exception& e) 
            {
                std::cerr << "Failed to start RabbitMQ receiver: " << e.what() << std::endl;
            }

            connectionHandler_->Start();

            // Tidy up after
            this->connection_->close();
            this->channel_.reset();
            this->connection_.reset();
            this->connectionHandler_.reset();
        }
    }

    std::string exchangeName_;
    std::string queueName_;
    std::string routingKey_;
    std::shared_ptr<AMQP::TcpConnection> connection_;
    std::shared_ptr<AMQP::TcpChannel> channel_;
    std::shared_ptr<ConnHandler> connectionHandler_;
    std::thread processingThread;
};

int main() 
{
    std::string exchangeName = "example_exchange";
    std::string queueName = "example_queue";
    std::string routingKey = "";

    std::thread receiverThread([&]() 
    {
        RabbitMQReceiver receiver(exchangeName, queueName, routingKey);

        // Wait for a message to be received
        while (true) 
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    std::thread transmitterThread([&]() 
    {
        RabbitMQTransmitter transmitter(exchangeName, queueName);
        while(true)
        {
            transmitter.SendMessage("Hello, world!");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });


    receiverThread.join();  
    transmitterThread.join();
}