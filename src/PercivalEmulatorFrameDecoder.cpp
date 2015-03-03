/*
 * PercivalEmulatorFrameDecoder.cpp
 *
 *  Created on: Feb 24, 2015
 *      Author: tcn45
 */

#include "PercivalEmulatorFrameDecoder.h"
#include <iostream>
#include <arpa/inet.h>

using namespace FrameReceiver;

PercivalEmulatorFrameDecoder::PercivalEmulatorFrameDecoder(LoggerPtr& logger) :
        FrameDecoder(logger),
		current_frame_seen_(-1),
		current_frame_buffer_id_(-1),
		current_frame_buffer_(0),
		current_frame_header_(0)
{
    current_packet_header_.reset(new uint8_t[sizeof(PercivalEmulatorFrameDecoder::PacketHeader)]);
    scratch_payload_buffer_.reset(new uint8_t[primary_packet_size]);
}

PercivalEmulatorFrameDecoder::~PercivalEmulatorFrameDecoder()
{
}

const size_t PercivalEmulatorFrameDecoder::get_frame_buffer_size(void) const
{
    const size_t partial_size = (num_primary_packets * primary_packet_size) + (num_tail_packets + tail_packet_size);
    const size_t buffer_size = (partial_size * num_subframes * 2) + get_frame_header_size();
    return buffer_size;
}

const size_t PercivalEmulatorFrameDecoder::get_frame_header_size(void) const
{
    return sizeof(PercivalEmulatorFrameDecoder::FrameHeader);
}

const size_t PercivalEmulatorFrameDecoder::get_packet_header_size(void) const
{
    return sizeof(PercivalEmulatorFrameDecoder::PacketHeader);
}

void* PercivalEmulatorFrameDecoder::get_packet_header_buffer(void)
{
    return current_packet_header_.get();
}

void PercivalEmulatorFrameDecoder::process_packet_header(size_t bytes_received)
{
    //TODO validate header size and content, handle incoming new packet buffer allocation etc

	uint32_t frame = get_frame_number();
	uint16_t packet = get_packet_number();
	uint8_t  subframe = get_subframe_number();
	uint8_t  type = get_packet_type();

    LOG4CXX_DEBUG(logger_, "Got packet header:"
            << " type: "     << (int)type << " subframe: " << (int)subframe
            << " packet: "   << packet    << " frame: "    << frame
    );
    dump_header();

    if (frame != current_frame_seen_)
    {
    	current_frame_seen_ = frame;

    	if (frame_buffer_map_.count(current_frame_seen_) == 0)
    	{
    		// TODO implement retry or error condition if empty buffer queue has no buffers available
    		current_frame_buffer_id_ = empty_buffer_queue_.front();
    		empty_buffer_queue_.pop();
    		frame_buffer_map_[current_frame_seen_] = current_frame_buffer_id_;

    		LOG4CXX_DEBUG(logger_, "First packet from frame " << current_frame_seen_ << " detected, allocating frame buffer ID " << current_frame_buffer_id_);

    		// TODO initialize frame buffer bookkeeping etc
        	current_frame_buffer_ = buffer_manager_->get_buffer_address(current_frame_buffer_id_);

        	current_frame_header_ = reinterpret_cast<FrameHeader*>(current_frame_buffer_);
        	current_frame_header_->frame_number = current_frame_seen_;
        	current_frame_header_->frame_state = FrameDecoder::FrameReceiveStateIncomplete;
        	current_frame_header_->packets_received = 0;
        	clock_gettime(CLOCK_REALTIME, reinterpret_cast<struct timespec*>(&(current_frame_header_->frame_start_time)));
    	}
    	else
    	{
    		current_frame_buffer_id_ = frame_buffer_map_[current_frame_seen_];
        	current_frame_buffer_ = buffer_manager_->get_buffer_address(current_frame_buffer_id_);
        	current_frame_header_ = reinterpret_cast<FrameHeader*>(current_frame_buffer_);
    	}

    }

}

void* PercivalEmulatorFrameDecoder::get_next_payload_buffer(void) const
{

    void* next_receive_location = scratch_payload_buffer_.get();
    return next_receive_location;
}

size_t PercivalEmulatorFrameDecoder::get_next_payload_size(void) const
{
   size_t next_receive_size = 0;

	if (get_packet_number() < num_primary_packets)
	{
		next_receive_size = primary_packet_size;
	}
	else
	{
		next_receive_size = tail_packet_size;
	}

    return next_receive_size;
}

FrameDecoder::FrameReceiveState PercivalEmulatorFrameDecoder::process_packet(size_t bytes_received)
{

	FrameDecoder::FrameReceiveState frame_state = FrameDecoder::FrameReceiveStateIncomplete;

	uint8_t* payload_raw = reinterpret_cast<uint8_t*>(scratch_payload_buffer_.get());

	for (int i = 0; i < 8; i++)
	{
		int idx = i;
		std::cout << std::hex << (int)payload_raw[idx] << std::dec << " ";
	}
	std::cout << std::endl << "..." << std::endl;;
	for (int i = 0; i < 8; i++)
	{
		int idx = primary_packet_size - (i+1);
		std::cout << std::hex << (int)payload_raw[idx] << std::dec << " ";
	}
	std::cout << std::endl;

	current_frame_header_->packets_received++;

	if (current_frame_header_->packets_received == (2 * 2 * 256))
	{
		LOG4CXX_DEBUG(logger_, "Frame " << current_frame_seen_ << " complete, releasing");
		frame_state = FrameDecoder::FrameReceiveStateComplete;
	}

	return frame_state;
}

uint8_t PercivalEmulatorFrameDecoder::get_packet_type(void) const
{
    return *(reinterpret_cast<uint8_t*>(raw_packet_header()+0));
}

uint8_t PercivalEmulatorFrameDecoder::get_subframe_number(void) const
{
    return *(reinterpret_cast<uint8_t*>(raw_packet_header()+1));
}

uint16_t PercivalEmulatorFrameDecoder::get_packet_number(void) const
{
	uint16_t packet_number_raw = *(reinterpret_cast<uint16_t*>(raw_packet_header()+6));
    return ntohs(packet_number_raw);
}

uint32_t PercivalEmulatorFrameDecoder::get_frame_number(void) const
{
	uint32_t frame_number_raw = *(reinterpret_cast<uint32_t*>(raw_packet_header()+2));
    return ntohl(frame_number_raw);
}

void PercivalEmulatorFrameDecoder::dump_header(void)
{
    uint8_t* raw_hdr = reinterpret_cast<uint8_t*>(current_packet_header_.get());

    for (int i = 0; i < sizeof(PercivalEmulatorFrameDecoder::PacketHeader); i++)
    {
        std::cout << std::hex << (int)raw_hdr[i] << std::dec << " ";
    }
    std::cout << std::endl;
}

uint8_t* PercivalEmulatorFrameDecoder::raw_packet_header(void) const
{
    return reinterpret_cast<uint8_t*>(current_packet_header_.get());
}
