/*!
 * FrameReceiverRxThreadUnitTest.cpp
 *
 *  Created on: Feb 5, 2015
 *      Author: Tim Nicholls, STFC Application Engineering Group
 */

#include <boost/test/unit_test.hpp>

#include "FrameReceiverRxThread.h"
#include "IpcMessage.h"
#include "SharedBufferManager.h"
#include "FrameDecoder.h"
#include "PercivalEmulatorFrameDecoder.h"

#include <log4cxx/logger.h>
#include <log4cxx/consoleappender.h>
#include <log4cxx/basicconfigurator.h>
#include <log4cxx/simplelayout.h>

namespace FrameReceiver
{
    class FrameReceiverRxThreadTestProxy
    {
    public:
        FrameReceiverRxThreadTestProxy(FrameReceiver::FrameReceiverConfig& config) :
            config_(config)
        {
            // Override default RX buffer size on OS X as Linux default
            // is too large for test to pass
#ifdef __MACH__
            config_.rx_recv_buffer_size_ = 1048576;
#endif
        }

        std::string& get_rx_channel_endpoint(void)
        {
            return config_.rx_channel_endpoint_;
        }
    private:
        FrameReceiver::FrameReceiverConfig& config_;
    };
}
class FrameReceiverRxThreadTestFixture
{
public:
    FrameReceiverRxThreadTestFixture() :
        rx_channel(ZMQ_PAIR),
        logger(log4cxx::Logger::getLogger("FrameReceiverRxThreadUnitTest")),
        proxy(config),
        frame_decoder(new FrameReceiver::PercivalEmulatorFrameDecoder(logger)),
        buffer_manager(new FrameReceiver::SharedBufferManager("TestSharedBuffer", 10000, 1000))
    {

        BOOST_TEST_MESSAGE("Setup test fixture");

        // Bind the endpoint of the channel to communicate with the RX thread
        rx_channel.bind(proxy.get_rx_channel_endpoint());

        // Create a log4cxx console appender so that thread messages can be printed, suppress debug messages
        log4cxx::ConsoleAppender* consoleAppender = new log4cxx::ConsoleAppender(log4cxx::LayoutPtr(new log4cxx::SimpleLayout()));
        log4cxx::helpers::Pool p;
        consoleAppender->activateOptions(p);
        log4cxx::BasicConfigurator::configure(log4cxx::AppenderPtr(consoleAppender));
        log4cxx::Logger::getRootLogger()->setLevel(log4cxx::Level::getInfo());
    }

    ~FrameReceiverRxThreadTestFixture()
    {
        BOOST_TEST_MESSAGE("Tear down test fixture");
    }

    FrameReceiver::IpcChannel rx_channel;
    FrameReceiver::FrameReceiverConfig config;
    log4cxx::LoggerPtr logger;
    FrameReceiver::FrameReceiverRxThreadTestProxy proxy;
    FrameReceiver::FrameDecoderPtr frame_decoder;
    FrameReceiver::SharedBufferManagerPtr buffer_manager;
};

BOOST_FIXTURE_TEST_SUITE(FrameReceiverRxThreadUnitTest, FrameReceiverRxThreadTestFixture);

BOOST_AUTO_TEST_CASE( CreateAndPingRxThread )
{

    bool initOK = true;

    try {
        FrameReceiver::FrameReceiverRxThread rxThread(config, logger, buffer_manager, frame_decoder, 1);

        FrameReceiver::IpcMessage::MsgType msg_type = FrameReceiver::IpcMessage::MsgTypeCmd;
        FrameReceiver::IpcMessage::MsgVal  msg_val =  FrameReceiver::IpcMessage::MsgValCmdStatus;

        int loopCount = 500;
        int replyCount = 0;
        int timeoutCount = 0;
        bool msgMatch = true;

        for (int loop = 0; loop < loopCount; loop++)
        {
            FrameReceiver::IpcMessage message(msg_type, msg_val);
            message.set_param<int>("count", loop);
            rx_channel.send(message.encode());
        }

        while ((replyCount < loopCount) && (timeoutCount < 10))
        {
            if (rx_channel.poll(100))
            {
                std::string reply = rx_channel.recv();

                FrameReceiver::IpcMessage response(reply.c_str());
                msgMatch &= (response.get_msg_type() == FrameReceiver::IpcMessage::MsgTypeAck);
                msgMatch &= (response.get_msg_val() == FrameReceiver::IpcMessage::MsgValCmdStatus);
                msgMatch &= (response.get_param<int>("count", -1) == replyCount);
                replyCount++;
                timeoutCount = 0;
            }
            else
            {
                timeoutCount++;
            }
        }

        BOOST_CHECK_EQUAL(msgMatch, true);
        BOOST_CHECK_EQUAL(loopCount, replyCount);
        BOOST_CHECK_EQUAL(timeoutCount, 0);
        }
    catch (FrameReceiver::FrameReceiverException& e)
    {
        initOK = false;
        BOOST_TEST_MESSAGE("Creation of FrameReceiverRxThread failed: " << e.what());
    }
    BOOST_REQUIRE_EQUAL(initOK, true);

}

BOOST_AUTO_TEST_SUITE_END();



