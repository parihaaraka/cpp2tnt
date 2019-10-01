#pragma once

#include <string_view>
#include <unordered_map>

#include "mp_writer.h"
#include "mp_reader.h"
#include "proto.h"
#include "connection.h"

using namespace std;

namespace tnt
{

    class Stream
    {
    public:

    };

    class Header
    {
    public:
        uint64_t sync;
        uint64_t errCode;
    };


    //template<typename... Args>
    //void write(){};

    class Connector : public connection, public iproto_writer
    {
    public:
        class FuncParamTuple
        {
        public:
            template<typename... Args>
            FuncParamTuple(Args... args)
            {
                //std::make_tuple(args...);
            }

        };

        typedef function<void(const Header& header, const mp_map_reader& body, void* userData)> OnFuncResult;
        typedef function<void()> SimpleEventCallbak;

        Connector();

        template<typename UserData = void*, typename... Args>
        void Call(string_view name, OnFuncResult&& resultHundler, UserData userData = (void*)nullptr, Args... args)
        {
            if (isConnected_)
            {
                static_assert (sizeof (HandlerData::userData_) >= sizeof (userData), "User data too big.");
                constexpr size_t countArgs = sizeof...(args);
                begin_call(name);
                begin_array(countArgs);
                write(args...);
				//if (countArgs)
                    finalize();
                finalize();
                HandlerData handler;
                handler.handler_ = move(resultHundler);
                *((decltype (userData)*)&handler.userData_) = move(userData);
                handlers_[last_request_id()] = move(handler);
                flush();
            }
            else
            {
                wtf_buffer buff;
                mp_writer writer(buff);
                writer.begin_map(1);
                writer<<int(tnt::response_field::ERROR)<<"disconected";
                writer.finalize();
                Header header;
                header.errCode = 77;
                header.sync = 0;
                mp_reader reader(buff);
                mp_map_reader body = reader.map();
                resultHundler(header, body, &userData);
            }
        }

        void write()
        {}

        template<typename T, typename... Args>
        void write(T& arg, Args... args)
        {
            *this<<arg;
            write(args...);
        }

        void AddOnOpened(SimpleEventCallbak cb_);
        void AddOnClosed(SimpleEventCallbak cb_);
        void close(bool reconnect_soon = false) noexcept;
        bool IsConnected();

    protected:
        class HandlerData
        {
        public:
            OnFuncResult handler_;
            uint64_t userData_[2];
        };

        unordered_map<uint64_t, HandlerData> handlers_;
        vector<SimpleEventCallbak> onOpenedHandlers_;
        vector<SimpleEventCallbak> onClosedHandlers_;
        bool isConnected_ = false;

        void OnResponse(wtf_buffer &buf);
        void OnOpened();
        void OnClosed();

        bool isNeedsClose_ = false;
        bool isNeedsReconnect_ = false;
        bool isProcessingReply_ = false;

    };


}
