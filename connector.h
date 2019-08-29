#pragma once

#include <string_view>
#include <map>

#include "tntevloop.h"
#include "mp_writer.h"
#include "mp_reader.h"

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

    class Connector : public TntEvLoop, public iproto_writer
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

        typedef fu2::unique_function<void(const Header& header, const mp_map_reader& body)> OnFuncResult;

        Connector();

        template<typename... Args>
        void Call(string_view name, OnFuncResult&& resultHundler, Args... args)
        {
            constexpr size_t countArgs = sizeof...(args);
            begin_call(name);
            begin_array(countArgs);
            write(args...);
            if (countArgs)
                finalize();
            finalize();
            handlers_[last_request_id()] = move(resultHundler);
            flush();
        }

        void write()
        {}

        template<typename T, typename... Args>
        void write(T arg, Args... args)
        {
            *this<<arg;
            write(args...);
        }

    protected:
        map<uint64_t, OnFuncResult> handlers_;

        void OnResponse(wtf_buffer &buf);

    };


}
