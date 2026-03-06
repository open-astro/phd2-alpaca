/*
 *  event_server.cpp
 *  PHD Guiding
 *
 *  Created by Andy Galasso.
 *  Copyright (c) 2013 Andy Galasso.
 *  All rights reserved.
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of Craig Stark, Stark Labs nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "phd.h"

#include <wx/sstream.h>
#include <wx/sckstrm.h>
#include <sstream>
#include <string.h>

#if defined(ALPACA_CAMERA) || defined(GUIDE_ALPACA) || defined(ROTATOR_ALPACA)
# include "alpaca_client.h"
# include "alpaca_discovery.h"
#endif

EventServer EvtServer;

// clang-format off
wxBEGIN_EVENT_TABLE(EventServer, wxEvtHandler)
    EVT_SOCKET(EVENT_SERVER_ID, EventServer::OnEventServerEvent)
    EVT_SOCKET(EVENT_SERVER_CLIENT_ID, EventServer::OnEventServerClientEvent)
wxEND_EVENT_TABLE();
// clang-format on

enum
{
    MSG_PROTOCOL_VERSION = 1,
};

static const wxString literal_null("null");
static const wxString literal_true("true");
static const wxString literal_false("false");

static wxString state_name(EXPOSED_STATE st)
{
    switch (st)
    {
    case EXPOSED_STATE_NONE:
        return "Stopped";
    case EXPOSED_STATE_SELECTED:
        return "Selected";
    case EXPOSED_STATE_CALIBRATING:
        return "Calibrating";
    case EXPOSED_STATE_GUIDING_LOCKED:
        return "Guiding";
    case EXPOSED_STATE_GUIDING_LOST:
        return "LostLock";
    case EXPOSED_STATE_PAUSED:
        return "Paused";
    case EXPOSED_STATE_LOOPING:
        return "Looping";
    default:
        return "Unknown";
    }
}

static wxString json_escape(const wxString& s)
{
    wxString t(s);
    static const wxString BACKSLASH("\\");
    static const wxString BACKSLASHBACKSLASH("\\\\");
    static const wxString DQUOT("\"");
    static const wxString BACKSLASHDQUOT("\\\"");
    static const wxString CR("\r");
    static const wxString BACKSLASHCR("\\r");
    static const wxString LF("\n");
    static const wxString BACKSLASHLF("\\n");
    t.Replace(BACKSLASH, BACKSLASHBACKSLASH);
    t.Replace(DQUOT, BACKSLASHDQUOT);
    t.Replace(CR, BACKSLASHCR);
    t.Replace(LF, BACKSLASHLF);
    return t;
}

template<char LDELIM, char RDELIM>
struct JSeq
{
    wxString m_s;
    bool m_first;
    bool m_closed;
    JSeq() : m_first(true), m_closed(false) { m_s << LDELIM; }
    void close()
    {
        m_s << RDELIM;
        m_closed = true;
    }
    wxString str()
    {
        if (!m_closed)
            close();
        return m_s;
    }
};

typedef JSeq<'[', ']'> JAry;
typedef JSeq<'{', '}'> JObj;

static JAry& operator<<(JAry& a, const wxString& str)
{
    if (a.m_first)
        a.m_first = false;
    else
        a.m_s << ',';
    a.m_s << str;
    return a;
}

static JAry& operator<<(JAry& a, double d)
{
    return a << wxString::Format("%.2f", d);
}

static JAry& operator<<(JAry& a, int i)
{
    return a << wxString::Format("%d", i);
}

static wxString json_format(const json_value *j)
{
    if (!j)
        return literal_null;

    switch (j->type)
    {
    default:
    case JSON_NULL:
        return literal_null;
    case JSON_OBJECT:
    {
        wxString ret("{");
        bool first = true;
        json_for_each(jj, j)
        {
            if (first)
                first = false;
            else
                ret << ",";
            ret << '"' << jj->name << "\":" << json_format(jj);
        }
        ret << "}";
        return ret;
    }
    case JSON_ARRAY:
    {
        wxString ret("[");
        bool first = true;
        json_for_each(jj, j)
        {
            if (first)
                first = false;
            else
                ret << ",";
            ret << json_format(jj);
        }
        ret << "]";
        return ret;
    }
    case JSON_STRING:
        return '"' + json_escape(j->string_value) + '"';
    case JSON_INT:
        return wxString::Format("%d", j->int_value);
    case JSON_FLOAT:
        return wxString::Format("%g", (double) j->float_value);
    case JSON_BOOL:
        return j->int_value ? literal_true : literal_false;
    }
}

struct NULL_TYPE
{
} NULL_VALUE;

// name-value pair
struct NV
{
    wxString n;
    wxString v;
    NV(const wxString& n_, const wxString& v_) : n(n_), v('"' + json_escape(v_) + '"') { }
    NV(const wxString& n_, const char *v_) : n(n_), v('"' + json_escape(v_) + '"') { }
    NV(const wxString& n_, const wchar_t *v_) : n(n_), v('"' + json_escape(v_) + '"') { }
    NV(const wxString& n_, int v_) : n(n_), v(wxString::Format("%d", v_)) { }
    NV(const wxString& n_, unsigned int v_) : n(n_), v(wxString::Format("%u", v_)) { }
    NV(const wxString& n_, double v_) : n(n_), v(wxString::Format("%g", v_)) { }
    NV(const wxString& n_, double v_, int prec) : n(n_), v(wxString::Format("%.*f", prec, v_)) { }
    NV(const wxString& n_, bool v_) : n(n_), v(v_ ? literal_true : literal_false) { }
    template<typename T>
    NV(const wxString& n_, const std::vector<T>& vec);
    NV(const wxString& n_, JAry& ary) : n(n_), v(ary.str()) { }
    NV(const wxString& n_, JObj& obj) : n(n_), v(obj.str()) { }
    NV(const wxString& n_, const json_value *v_) : n(n_), v(json_format(v_)) { }
    NV(const wxString& n_, const PHD_Point& p) : n(n_)
    {
        JAry ary;
        ary << p.X << p.Y;
        v = ary.str();
    }
    NV(const wxString& n_, const wxPoint& p) : n(n_)
    {
        JAry ary;
        ary << p.x << p.y;
        v = ary.str();
    }
    NV(const wxString& n_, const wxSize& s) : n(n_)
    {
        JAry ary;
        ary << s.x << s.y;
        v = ary.str();
    }
    NV(const wxString& n_, const wxRect& r) : n(n_)
    {
        JAry ary;
        ary << r.x << r.y << r.width << r.height;
        v = ary.str();
    }
    NV(const wxString& n_, const NULL_TYPE& nul) : n(n_), v(literal_null) { }
};

template<typename T>
NV::NV(const wxString& n_, const std::vector<T>& vec) : n(n_)
{
    std::ostringstream os;
    os << '[';
    for (unsigned int i = 0; i < vec.size(); i++)
    {
        if (i != 0)
            os << ',';
        os << vec[i];
    }
    os << ']';
    v = os.str();
}

static JObj& operator<<(JObj& j, const NV& nv)
{
    if (j.m_first)
        j.m_first = false;
    else
        j.m_s << ',';
    j.m_s << '"' << nv.n << "\":" << nv.v;
    return j;
}

static NV NVMount(const Mount *mount)
{
    return NV("Mount", mount->Name());
}

static JObj& operator<<(JObj& j, const PHD_Point& pt)
{
    return j << NV("X", pt.X, 3) << NV("Y", pt.Y, 3);
}

static JAry& operator<<(JAry& a, JObj& j)
{
    return a << j.str();
}

struct Ev : public JObj
{
    Ev(const wxString& event)
    {
        double const now = ::wxGetUTCTimeMillis().ToDouble() / 1000.0;
        *this << NV("Event", event) << NV("Timestamp", now, 3) << NV("Host", wxGetHostName())
              << NV("Inst", wxGetApp().GetInstanceNumber());
    }
};

static Ev ev_message_version()
{
    Ev ev("Version");
    ev << NV("PHDVersion", PHDVERSION) << NV("PHDSubver", PHDSUBVER) << NV("OverlapSupport", true)
       << NV("MsgVersion", MSG_PROTOCOL_VERSION);
    return ev;
}

static Ev ev_set_lock_position(const PHD_Point& xy)
{
    Ev ev("LockPositionSet");
    ev << xy;
    return ev;
}

static Ev ev_calibration_complete(const Mount *mount)
{
    Ev ev("CalibrationComplete");
    ev << NVMount(mount);

    if (mount->IsStepGuider())
    {
        ev << NV("Limit", mount->GetAoMaxPos());
    }

    return ev;
}

static Ev ev_star_selected(const PHD_Point& pos)
{
    Ev ev("StarSelected");
    ev << pos;
    return ev;
}

static Ev ev_start_guiding()
{
    return Ev("StartGuiding");
}

static Ev ev_paused()
{
    return Ev("Paused");
}

static Ev ev_start_calibration(const Mount *mount)
{
    Ev ev("StartCalibration");
    ev << NVMount(mount);
    return ev;
}

static Ev ev_app_state(EXPOSED_STATE st = Guider::GetExposedState())
{
    Ev ev("AppState");
    ev << NV("State", state_name(st));
    return ev;
}

static Ev ev_settling(double distance, double time, double settleTime, bool starLocked)
{
    Ev ev("Settling");

    ev << NV("Distance", distance, 2) << NV("Time", time, 1) << NV("SettleTime", settleTime, 1) << NV("StarLocked", starLocked);

    return ev;
}

static Ev ev_settle_done(const wxString& errorMsg, int settleFrames, int droppedFrames)
{
    Ev ev("SettleDone");

    int status = errorMsg.IsEmpty() ? 0 : 1;

    ev << NV("Status", status);

    if (status != 0)
    {
        ev << NV("Error", errorMsg);
    }

    ev << NV("TotalFrames", settleFrames) << NV("DroppedFrames", droppedFrames);

    return ev;
}

struct ClientReadBuf
{
    enum
    {
        SIZE = 1024
    };
    char m_buf[SIZE];
    char *dest;

    ClientReadBuf() { reset(); }
    char *buf() { return &m_buf[0]; }
    size_t len() const { return dest - &m_buf[0]; }
    size_t avail() const { return &m_buf[SIZE] - dest; }
    void reset() { dest = &m_buf[0]; }
};

struct ClientData
{
    wxSocketClient *cli;
    int refcnt;
    ClientReadBuf rdbuf;
    wxMutex wrlock;

    ClientData(wxSocketClient *cli_) : cli(cli_), refcnt(1) { }
    void AddRef() { ++refcnt; }
    void RemoveRef()
    {
        if (--refcnt == 0)
        {
            cli->Destroy();
            delete this;
        }
    }
};

struct ClientDataGuard
{
    ClientData *cd;
    ClientDataGuard(wxSocketClient *cli) : cd((ClientData *) cli->GetClientData()) { cd->AddRef(); }
    ~ClientDataGuard() { cd->RemoveRef(); }
    ClientData *operator->() const { return cd; }
};

inline static wxMutex *client_wrlock(wxSocketClient *cli)
{
    return &((ClientData *) cli->GetClientData())->wrlock;
}

static wxString SockErrStr(wxSocketError e)
{
    switch (e)
    {
    case wxSOCKET_NOERROR:
        return "";
    case wxSOCKET_INVOP:
        return "Invalid operation";
    case wxSOCKET_IOERR:
        return "Input / Output error";
    case wxSOCKET_INVADDR:
        return "Invalid address";
    case wxSOCKET_INVSOCK:
        return "Invalid socket(uninitialized)";
    case wxSOCKET_NOHOST:
        return "No corresponding host";
    case wxSOCKET_INVPORT:
        return "Invalid port";
    case wxSOCKET_WOULDBLOCK:
        return "operation would block";
    case wxSOCKET_TIMEDOUT:
        return "timeout expired";
    case wxSOCKET_MEMERR:
        return "Memory exhausted";
    default:
        return wxString::Format("unknown socket error %d", e);
    }
}

static void send_buf(wxSocketClient *client, const wxCharBuffer& buf)
{
    wxMutexLocker lock(*client_wrlock(client));
    client->Write(buf.data(), buf.length());
    if (client->LastWriteCount() != buf.length())
    {
        Debug.Write(wxString::Format("evsrv: cli %p short write %u/%u %s\n", client, client->LastWriteCount(),
                                     (unsigned int) buf.length(),
                                     SockErrStr(client->Error() ? client->LastError() : wxSOCKET_NOERROR)));
    }
}

static void do_notify1(wxSocketClient *client, const JAry& ary)
{
    send_buf(client, (JAry(ary).str() + "\r\n").ToUTF8());
}

static void do_notify1(wxSocketClient *client, const JObj& j)
{
    send_buf(client, (JObj(j).str() + "\r\n").ToUTF8());
}

static void do_notify(const EventServer::CliSockSet& cli, const JObj& jj)
{
    wxCharBuffer buf = (JObj(jj).str() + "\r\n").ToUTF8();

    for (EventServer::CliSockSet::const_iterator it = cli.begin(); it != cli.end(); ++it)
    {
        send_buf(*it, buf);
    }
}

inline static void simple_notify(const EventServer::CliSockSet& cli, const wxString& ev)
{
    if (!cli.empty())
        do_notify(cli, Ev(ev));
}

inline static void simple_notify_ev(const EventServer::CliSockSet& cli, const Ev& ev)
{
    if (!cli.empty())
        do_notify(cli, ev);
}

#define SIMPLE_NOTIFY(s) simple_notify(m_eventServerClients, s)
#define SIMPLE_NOTIFY_EV(ev) simple_notify_ev(m_eventServerClients, ev)

static void send_catchup_events(wxSocketClient *cli)
{
    EXPOSED_STATE st = Guider::GetExposedState();

    do_notify1(cli, ev_message_version());

    if (pFrame->pGuider)
    {
        if (pFrame->pGuider->LockPosition().IsValid())
            do_notify1(cli, ev_set_lock_position(pFrame->pGuider->LockPosition()));

        if (pFrame->pGuider->CurrentPosition().IsValid())
            do_notify1(cli, ev_star_selected(pFrame->pGuider->CurrentPosition()));
    }

    if (pMount && pMount->IsCalibrated())
        do_notify1(cli, ev_calibration_complete(pMount));

    if (pSecondaryMount && pSecondaryMount->IsCalibrated())
        do_notify1(cli, ev_calibration_complete(pSecondaryMount));

    if (st == EXPOSED_STATE_GUIDING_LOCKED)
    {
        do_notify1(cli, ev_start_guiding());
    }
    else if (st == EXPOSED_STATE_CALIBRATING)
    {
        Mount *mount = pMount;
        if (pFrame->pGuider->GetState() == STATE_CALIBRATING_SECONDARY)
            mount = pSecondaryMount;
        do_notify1(cli, ev_start_calibration(mount));
    }
    else if (st == EXPOSED_STATE_PAUSED)
    {
        do_notify1(cli, ev_paused());
    }

    do_notify1(cli, ev_app_state());
}

static void destroy_client(wxSocketClient *cli)
{
    ClientData *buf = (ClientData *) cli->GetClientData();
    buf->RemoveRef();
}

static void drain_input(wxSocketInputStream& sis)
{
    while (sis.CanRead())
    {
        char buf[1024];
        if (sis.Read(buf, sizeof(buf)).LastRead() == 0)
            break;
    }
}

enum
{
    JSONRPC_PARSE_ERROR = -32700,
    JSONRPC_INVALID_REQUEST = -32600,
    JSONRPC_METHOD_NOT_FOUND = -32601,
    JSONRPC_INVALID_PARAMS = -32602,
    JSONRPC_INTERNAL_ERROR = -32603,
};

static NV jrpc_error(int code, const wxString& msg)
{
    JObj err;
    err << NV("code", code) << NV("message", msg);
    return NV("error", err);
}

template<typename T>
static NV jrpc_result(const T& t)
{
    return NV("result", t);
}

template<typename T>
static NV jrpc_result(T& t)
{
    return NV("result", t);
}

static NV jrpc_id(const json_value *id)
{
    return NV("id", id);
}

struct JRpcResponse : public JObj
{
    JRpcResponse() { *this << NV("jsonrpc", "2.0"); }
};

static wxString parser_error(const JsonParser& parser)
{
    return wxString::Format("invalid JSON request: %s on line %d at \"%.12s...\"", parser.ErrorDesc(), parser.ErrorLine(),
                            parser.ErrorPos());
}

static void parse_request(const json_value *req, const json_value **pmethod, const json_value **pparams, const json_value **pid)
{
    *pmethod = *pparams = *pid = 0;

    if (req)
    {
        json_for_each(t, req)
        {
            if (t->name)
            {
                if (t->type == JSON_STRING && strcmp(t->name, "method") == 0)
                    *pmethod = t;
                else if (strcmp(t->name, "params") == 0)
                    *pparams = t;
                else if (strcmp(t->name, "id") == 0)
                    *pid = t;
            }
        }
    }
}

// paranoia
#define VERIFY_GUIDER(response)                                                                                                \
    do                                                                                                                         \
    {                                                                                                                          \
        if (!pFrame || !pFrame->pGuider)                                                                                       \
        {                                                                                                                      \
            response << jrpc_error(1, "internal error");                                                                       \
            return;                                                                                                            \
        }                                                                                                                      \
    } while (0)

static void deselect_star(JObj& response, const json_value *params)
{
    VERIFY_GUIDER(response);
    pFrame->pGuider->Reset(true);
    response << jrpc_result(0);
}

static void get_exposure(JObj& response, const json_value *params)
{
    response << jrpc_result(pFrame->RequestedExposureDuration());
}

static void get_exposure_durations(JObj& response, const json_value *params)
{
    const std::vector<int>& exposure_durations = pFrame->GetExposureDurations();
    response << jrpc_result(exposure_durations);
}

static void get_profiles(JObj& response, const json_value *params)
{
    JAry ary;
    wxArrayString names = pConfig->ProfileNames();
    for (unsigned int i = 0; i < names.size(); i++)
    {
        wxString name = names[i];
        int id = pConfig->GetProfileId(name);
        if (id)
        {
            JObj t;
            t << NV("id", id) << NV("name", name);
            if (id == pConfig->GetCurrentProfileId())
                t << NV("selected", true);
            ary << t;
        }
    }
    response << jrpc_result(ary);
}

struct Params
{
    std::map<std::string, const json_value *> dict;

    void Init(const char *names[], size_t nr_names, const json_value *params)
    {
        if (!params)
            return;
        if (params->type == JSON_ARRAY)
        {
            const json_value *jv = params->first_child;
            for (size_t i = 0; jv && i < nr_names; i++, jv = jv->next_sibling)
            {
                const char *name = names[i];
                dict.insert(std::make_pair(std::string(name), jv));
            }
        }
        else if (params->type == JSON_OBJECT)
        {
            json_for_each(jv, params)
            {
                dict.insert(std::make_pair(std::string(jv->name), jv));
            }
        }
    }
    Params(const char *n1, const json_value *params)
    {
        const char *n[] = { n1 };
        Init(n, 1, params);
    }
    Params(const char *n1, const char *n2, const json_value *params)
    {
        const char *n[] = { n1, n2 };
        Init(n, 2, params);
    }
    Params(const char *n1, const char *n2, const char *n3, const json_value *params)
    {
        const char *n[] = { n1, n2, n3 };
        Init(n, 3, params);
    }
    Params(const char *n1, const char *n2, const char *n3, const char *n4, const json_value *params)
    {
        const char *n[] = { n1, n2, n3, n4 };
        Init(n, 4, params);
    }
    Params(const char *n1, const char *n2, const char *n3, const char *n4, const char *n5, const json_value *params)
    {
        const char *n[] = { n1, n2, n3, n4, n5 };
        Init(n, 5, params);
    }
    Params(const char *n1, const char *n2, const char *n3, const char *n4, const char *n5, const char *n6,
           const json_value *params)
    {
        const char *n[] = { n1, n2, n3, n4, n5, n6 };
        Init(n, 6, params);
    }
    const json_value *param(const std::string& name) const
    {
        auto it = dict.find(name);
        return it == dict.end() ? 0 : it->second;
    }
};

static void set_exposure(JObj& response, const json_value *params)
{
    Params p("exposure", params);
    const json_value *exp = p.param("exposure");

    if (!exp || exp->type != JSON_INT)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected exposure param");
        return;
    }

    bool ok = pFrame->SetExposureDuration(exp->int_value);
    if (ok)
    {
        response << jrpc_result(0);
    }
    else
    {
        response << jrpc_error(1, "could not set exposure duration");
    }
}

static void get_profile(JObj& response, const json_value *params)
{
    int id = pConfig->GetCurrentProfileId();
    wxString name = pConfig->GetCurrentProfile();
    JObj t;
    t << NV("id", id) << NV("name", name);
    response << jrpc_result(t);
}

inline static void devstat(JObj& t, const char *dev, const wxString& name, bool connected)
{
    JObj o;
    t << NV(dev, o << NV("name", name) << NV("connected", connected));
}

static void get_current_equipment(JObj& response, const json_value *params)
{
    JObj t;

    if (pCamera)
        devstat(t, "camera", pCamera->Name, pCamera->Connected);

    Mount *mount = TheScope();
    if (mount)
        devstat(t, "mount", mount->Name(), mount->IsConnected());

    Mount *auxMount = pFrame->pGearDialog->AuxScope();
    if (auxMount)
        devstat(t, "aux_mount", auxMount->Name(), auxMount->IsConnected());

    Mount *ao = TheAO();
    if (ao)
        devstat(t, "AO", ao->Name(), ao->IsConnected());

    Rotator *rotator = pRotator;
    if (rotator)
        devstat(t, "rotator", rotator->Name(), rotator->IsConnected());

    response << jrpc_result(t);
}

static JAry json_string_array(const wxArrayString& choices)
{
    JAry ary;
    for (unsigned int i = 0; i < choices.Count(); i++)
        ary << ('"' + json_escape(choices[i]) + '"');
    return ary;
}

static bool DeviceSelectionMatches(const wxString& val, const wxString& item)
{
    if (val.Contains("INDI"))
        return item.Contains("INDI");
    return val == item;
}

static bool FindMatchingChoice(const wxArrayString& choices, const wxString& requested, wxString *choice)
{
    if (requested.IsEmpty())
        return false;

    for (unsigned int i = 0; i < choices.Count(); i++)
    {
        if (choices[i] == requested)
        {
            *choice = choices[i];
            return true;
        }
    }

    for (unsigned int i = 0; i < choices.Count(); i++)
    {
        if (choices[i].CmpNoCase(requested) == 0)
        {
            *choice = choices[i];
            return true;
        }
    }

    for (unsigned int i = 0; i < choices.Count(); i++)
    {
        if (DeviceSelectionMatches(requested, choices[i]))
        {
            *choice = choices[i];
            return true;
        }
    }

    if (requested.CmpNoCase("INDI") == 0 || requested.CmpNoCase("INDI Camera") == 0 || requested.CmpNoCase("INDI Mount") == 0)
    {
        for (unsigned int i = 0; i < choices.Count(); i++)
        {
            if (choices[i].Contains("INDI"))
            {
                *choice = choices[i];
                return true;
            }
        }
    }

    if (requested.CmpNoCase("Alpaca") == 0)
    {
        for (unsigned int i = 0; i < choices.Count(); i++)
        {
            if (choices[i].Contains("Alpaca"))
            {
                *choice = choices[i];
                return true;
            }
        }
    }

    return false;
}

static bool any_equipment_connected()
{
    return (pCamera && pCamera->Connected) || (pMount && pMount->IsConnected()) ||
           (pSecondaryMount && pSecondaryMount->IsConnected()) || (pRotator && pRotator->IsConnected()) ||
           (TheAO() && TheAO()->IsConnected()) || (pFrame->pGearDialog->AuxScope() && pFrame->pGearDialog->AuxScope()->IsConnected());
}

static wxString extract_bracketed_driver_name(const wxString& selection)
{
    int lbracket = selection.Find('[');
    int rbracket = selection.Find(']', true);
    if (lbracket == wxNOT_FOUND || rbracket == wxNOT_FOUND || rbracket <= lbracket + 1)
        return wxEmptyString;
    return selection.Mid(lbracket + 1, rbracket - lbracket - 1);
}

static void apply_selection_to_control(int choiceControlId, const wxArrayString& choices, const wxString& selectedChoice)
{
    wxWindow *wnd = pFrame->pGearDialog->FindWindow(choiceControlId);
    wxChoice *ctrl = wxDynamicCast(wnd, wxChoice);
    if (!ctrl)
        return;

    ctrl->Freeze();
    ctrl->Clear();
    ctrl->Append(choices);
    ctrl->SetStringSelection(selectedChoice);
    ctrl->Thaw();

    wxCommandEvent evt(wxEVT_CHOICE, choiceControlId);
    evt.SetEventObject(ctrl);
    evt.SetString(selectedChoice);
    pFrame->pGearDialog->GetEventHandler()->ProcessEvent(evt);
}

static wxString selected_camera_choice()
{
    wxString camera = pConfig->Profile.GetString("/camera/LastMenuChoice", wxEmptyString);
    if (camera.IsEmpty())
        camera = pConfig->Profile.GetString("/camera/LastMenuchoice", _("None"));
    if (camera.IsEmpty())
        camera = _("None");
    return camera;
}

static void get_equipment_choices(JObj& response, const json_value *params)
{
    JObj t;
    JAry cameras = json_string_array(GuideCamera::GuideCameraList());
    JAry mounts = json_string_array(Scope::MountList());
    JAry auxMounts = json_string_array(Scope::AuxMountList());
    JAry aos = json_string_array(StepGuider::AOList());
    JAry rotators = json_string_array(Rotator::RotatorList());

    t << NV("camera", cameras) << NV("mount", mounts) << NV("aux_mount", auxMounts) << NV("AO", aos) << NV("rotator", rotators);
    response << jrpc_result(t);
}

static bool json_to_long(const json_value *val, long *out)
{
    if (!val)
        return false;
    if (val->type == JSON_INT)
    {
        *out = static_cast<long>(val->int_value);
        return true;
    }
    if (val->type == JSON_FLOAT)
    {
        *out = static_cast<long>(val->float_value);
        return true;
    }
    return false;
}

static bool parse_device_number_from_display(const wxString& display, long *deviceNumber)
{
    // Expected format: "Device <n>: <name>"
    wxString s = display;
    s.Trim(true).Trim(false);
    if (!s.StartsWith("Device "))
        return false;
    s = s.Mid(7);
    long n = 0;
    if (s.BeforeFirst(':').ToLong(&n))
    {
        *deviceNumber = n;
        return true;
    }
    return false;
}

static void get_indi_server(JObj& response, const json_value *params)
{
    JObj t;
    t << NV("host", pConfig->Profile.GetString("/indi/INDIhost", _("localhost")))
      << NV("port", static_cast<int>(pConfig->Profile.GetLong("/indi/INDIport", 7624)));
    response << jrpc_result(t);
}

static void set_indi_server(JObj& response, const json_value *params)
{
    Params p("host", "port", params);
    const json_value *host = p.param("host");
    const json_value *port = p.param("port");

    if (any_equipment_connected())
    {
        response << jrpc_error(1, "cannot change INDI server while equipment is connected");
        return;
    }

    if (!host && !port)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected host and/or port param");
        return;
    }

    if (host)
    {
        if (host->type != JSON_STRING || wxString(host->string_value).IsEmpty())
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected non-empty host string");
            return;
        }
        pConfig->Profile.SetString("/indi/INDIhost", host->string_value);
    }

    if (port)
    {
        long indiPort = 0;
        if (!json_to_long(port, &indiPort) || indiPort < 1 || indiPort > 65535)
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected INDI port in range 1..65535");
            return;
        }
        pConfig->Profile.SetLong("/indi/INDIport", indiPort);
    }

    pConfig->Flush();
    response << jrpc_result(0);
}

static void get_alpaca_server(JObj& response, const json_value *params)
{
    JObj t;
    t << NV("host", pConfig->Profile.GetString("/alpaca/host", _("localhost")))
      << NV("port", static_cast<int>(pConfig->Profile.GetLong("/alpaca/port", 6800)))
      << NV("camera_device", static_cast<int>(pConfig->Profile.GetLong("/alpaca/camera_device", 0)))
      << NV("telescope_device", static_cast<int>(pConfig->Profile.GetLong("/alpaca/telescope_device", 0)))
      << NV("rotator_device", static_cast<int>(pConfig->Profile.GetLong("/alpaca/rotator_device", 0)));
    response << jrpc_result(t);
}

static void apply_alpaca_choice_if_selected()
{
    wxString host = pConfig->Profile.GetString("/alpaca/host", _("localhost"));
    long port = pConfig->Profile.GetLong("/alpaca/port", 6800);
    long cameraDev = pConfig->Profile.GetLong("/alpaca/camera_device", 0);
    long mountDev = pConfig->Profile.GetLong("/alpaca/telescope_device", 0);
    long rotatorDev = pConfig->Profile.GetLong("/alpaca/rotator_device", 0);

    wxArrayString camChoices = GuideCamera::GuideCameraList();
    wxArrayString mountChoices = Scope::MountList();
    wxArrayString rotChoices = Rotator::RotatorList();
    wxString resolved;

    wxString selCam = selected_camera_choice();
    if (selCam.Contains("Alpaca"))
    {
        wxString target = wxString::Format("Alpaca Camera [%s:%ld/%ld]", host, port, cameraDev);
        if (FindMatchingChoice(camChoices, target, &resolved))
        {
            pConfig->Profile.SetString("/camera/LastMenuChoice", resolved);
            apply_selection_to_control(GEAR_CHOICE_CAMERA, camChoices, resolved);
        }
    }

    wxString selMount = pConfig->Profile.GetString("/scope/LastMenuChoice", _("None"));
    if (selMount.Contains("Alpaca"))
    {
        wxString target = wxString::Format(_("Alpaca Mount [%s:%ld/%ld]"), host, port, mountDev);
        if (FindMatchingChoice(mountChoices, target, &resolved))
        {
            pConfig->Profile.SetString("/scope/LastMenuChoice", resolved);
            apply_selection_to_control(GEAR_CHOICE_SCOPE, mountChoices, resolved);
        }
    }

    wxString selRot = pConfig->Profile.GetString("/rotator/LastMenuChoice", _("None"));
    if (selRot.Contains("Alpaca"))
    {
        wxString target = wxString::Format("Alpaca Rotator [%s:%ld/%ld]", host, port, rotatorDev);
        if (FindMatchingChoice(rotChoices, target, &resolved))
        {
            pConfig->Profile.SetString("/rotator/LastMenuChoice", resolved);
            apply_selection_to_control(GEAR_CHOICE_ROTATOR, rotChoices, resolved);
        }
    }
}

static void set_alpaca_server(JObj& response, const json_value *params)
{
    Params p("host", "port", "camera_device", "telescope_device", "rotator_device", params);
    const json_value *host = p.param("host");
    const json_value *port = p.param("port");
    const json_value *cameraDevice = p.param("camera_device");
    const json_value *telescopeDevice = p.param("telescope_device");
    const json_value *rotatorDevice = p.param("rotator_device");

    if (any_equipment_connected())
    {
        response << jrpc_error(1, "cannot change Alpaca server while equipment is connected");
        return;
    }

    if (!host && !port && !cameraDevice && !telescopeDevice && !rotatorDevice)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS,
                               "expected host/port and/or camera_device/telescope_device/rotator_device params");
        return;
    }

    if (host)
    {
        if (host->type != JSON_STRING || wxString(host->string_value).IsEmpty())
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected non-empty host string");
            return;
        }
        pConfig->Profile.SetString("/alpaca/host", host->string_value);
    }

    if (port)
    {
        long alpacaPort = 0;
        if (!json_to_long(port, &alpacaPort) || alpacaPort < 1 || alpacaPort > 65535)
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected Alpaca port in range 1..65535");
            return;
        }
        pConfig->Profile.SetLong("/alpaca/port", alpacaPort);
    }

    auto setDeviceNum = [&](const json_value *val, const char *key, const char *errName) -> bool
    {
        if (!val)
            return true;
        long n = 0;
        if (!json_to_long(val, &n) || n < 0)
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, wxString::Format("expected %s >= 0", errName));
            return false;
        }
        pConfig->Profile.SetLong(key, n);
        return true;
    };

    if (!setDeviceNum(cameraDevice, "/alpaca/camera_device", "camera_device"))
        return;
    if (!setDeviceNum(telescopeDevice, "/alpaca/telescope_device", "telescope_device"))
        return;
    if (!setDeviceNum(rotatorDevice, "/alpaca/rotator_device", "rotator_device"))
        return;

    apply_alpaca_choice_if_selected();
    pConfig->Flush();
    response << jrpc_result(0);
}

static bool alpaca_device_type_matches(const wxString& requestedType, const wxString& deviceType)
{
    wxString req = requestedType;
    req.MakeUpper();
    wxString dtype = deviceType;
    dtype.MakeUpper();

    if (req.IsEmpty() || req == "ALL")
        return true;
    if (req == "CAMERA")
        return dtype == "CAMERA";
    if (req == "TELESCOPE" || req == "MOUNT")
        return dtype == "TELESCOPE" || dtype == "MOUNT";
    if (req == "ROTATOR")
        return dtype == "ROTATOR";
    return false;
}

static void discover_alpaca_servers(JObj& response, const json_value *params)
{
#if defined(ALPACA_CAMERA) || defined(GUIDE_ALPACA) || defined(ROTATOR_ALPACA)
    Params p("num_queries", "queries", "timeout_seconds", "timeout", params);
    int numQueries = 2;
    int timeoutSeconds = 2;
    long val = 0;
    const json_value *jv = p.param("num_queries");
    if (!jv)
        jv = p.param("queries");
    if (jv)
    {
        if (!json_to_long(jv, &val) || val < 1 || val > 20)
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "num_queries must be in range 1..20");
            return;
        }
        numQueries = static_cast<int>(val);
    }
    jv = p.param("timeout_seconds");
    if (!jv)
        jv = p.param("timeout");
    if (jv)
    {
        if (!json_to_long(jv, &val) || val < 1 || val > 30)
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "timeout_seconds must be in range 1..30");
            return;
        }
        timeoutSeconds = static_cast<int>(val);
    }

    wxArrayString servers = AlpacaDiscovery::DiscoverServers(numQueries, timeoutSeconds);
    JAry serverArray = json_string_array(servers);
    response << jrpc_result(serverArray);
#else
    response << jrpc_error(1, "Alpaca support is not enabled in this build");
#endif
}

static void query_alpaca_devices(JObj& response, const json_value *params)
{
#if defined(ALPACA_CAMERA) || defined(GUIDE_ALPACA) || defined(ROTATOR_ALPACA)
    Params p("host", "port", "device_type", "type", params);
    const json_value *hostv = p.param("host");
    const json_value *portv = p.param("port");
    const json_value *typev = p.param("device_type");
    if (!typev)
        typev = p.param("type");

    wxString host = pConfig->Profile.GetString("/alpaca/host", _("localhost"));
    long port = pConfig->Profile.GetLong("/alpaca/port", 6800);
    wxString requestedType = "ALL";

    if (hostv)
    {
        if (hostv->type != JSON_STRING || wxString(hostv->string_value).IsEmpty())
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected non-empty host string");
            return;
        }
        host = hostv->string_value;
    }
    if (portv)
    {
        if (!json_to_long(portv, &port) || port < 1 || port > 65535)
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected port in range 1..65535");
            return;
        }
    }
    if (typev)
    {
        if (typev->type != JSON_STRING)
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected device_type string");
            return;
        }
        requestedType = wxString(typev->string_value).Upper();
        if (!(requestedType == "ALL" || requestedType == "CAMERA" || requestedType == "TELESCOPE" ||
              requestedType == "MOUNT" || requestedType == "ROTATOR"))
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "device_type must be one of: all,camera,telescope,mount,rotator");
            return;
        }
    }

    AlpacaClient client(host, port, 0);
    JsonParser parser;
    long errorCode = 0;
    if (!client.Get("management/v1/configureddevices", parser, &errorCode))
    {
        response << jrpc_error(1, wxString::Format("failed to query configured devices from %s:%ld (error %ld)", host, port,
                                                   errorCode));
        return;
    }

    const json_value *root = parser.Root();
    if (!root)
    {
        response << jrpc_error(1, "invalid response from server");
        return;
    }

    const json_value *valueArray = nullptr;
    if (root->type == JSON_OBJECT)
    {
        json_for_each(n, root)
        {
            if (n->name && strcmp(n->name, "Value") == 0 && n->type == JSON_ARRAY)
            {
                valueArray = n;
                break;
            }
        }
    }
    else if (root->type == JSON_ARRAY)
    {
        valueArray = root;
    }

    if (!valueArray || valueArray->type != JSON_ARRAY)
    {
        response << jrpc_error(1, "invalid response from server");
        return;
    }

    JAry devices;
    json_for_each(deviceNode, valueArray)
    {
        if (deviceNode->type != JSON_OBJECT)
            continue;

        long deviceNum = -1;
        wxString deviceType;
        wxString deviceName;

        json_for_each(prop, deviceNode)
        {
            if (!prop->name)
                continue;
            wxString propName(prop->name, wxConvUTF8);
            if (propName.CmpNoCase("DeviceNumber") == 0)
            {
                long n = 0;
                if (json_to_long(prop, &n))
                    deviceNum = n;
            }
            else if (propName.CmpNoCase("DeviceType") == 0 || propName.CmpNoCase("Type") == 0)
            {
                if (prop->type == JSON_STRING)
                    deviceType = wxString(prop->string_value, wxConvUTF8);
            }
            else if (propName.CmpNoCase("DeviceName") == 0 || propName.CmpNoCase("Name") == 0)
            {
                if (prop->type == JSON_STRING)
                    deviceName = wxString(prop->string_value, wxConvUTF8);
            }
        }

        if (deviceNum < 0)
            continue;
        if (!alpaca_device_type_matches(requestedType, deviceType))
            continue;

        wxString display = deviceName.IsEmpty() ? wxString::Format(_("Device %ld"), deviceNum) : deviceName;
        JObj d;
        d << NV("device_number", static_cast<int>(deviceNum)) << NV("device_type", deviceType) << NV("device_name", deviceName)
          << NV("display_name", display) << NV("display", wxString::Format("Device %ld: %s", deviceNum, display));
        devices << d;
    }

    response << jrpc_result(devices);
#else
    response << jrpc_error(1, "Alpaca support is not enabled in this build");
#endif
}

static void set_selected_alpaca_device(JObj& response, const json_value *params)
{
    Params p("device_type", "type", "device_number", "device", "display", params);
    const json_value *typev = p.param("device_type");
    if (!typev)
        typev = p.param("type");
    const json_value *numv = p.param("device_number");
    if (!numv)
        numv = p.param("device");
    const json_value *displayv = p.param("display");

    if (!typev || typev->type != JSON_STRING)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected device_type string");
        return;
    }
    if (any_equipment_connected())
    {
        response << jrpc_error(1, "cannot change Alpaca selection while equipment is connected");
        return;
    }

    wxString type = wxString(typev->string_value).Upper();
    if (!(type == "CAMERA" || type == "TELESCOPE" || type == "MOUNT" || type == "ROTATOR"))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "device_type must be one of: camera,telescope,mount,rotator");
        return;
    }

    long deviceNum = -1;
    if (numv)
    {
        if (!json_to_long(numv, &deviceNum) || deviceNum < 0)
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected device_number >= 0");
            return;
        }
    }
    else if (displayv)
    {
        if (displayv->type != JSON_STRING || !parse_device_number_from_display(displayv->string_value, &deviceNum))
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "could not parse device number from display");
            return;
        }
    }
    else
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected device_number or display param");
        return;
    }

    if (type == "CAMERA")
    {
        pConfig->Profile.SetLong("/alpaca/camera_device", deviceNum);
        wxArrayString choices = GuideCamera::GuideCameraList();
        wxString target = wxString::Format("Alpaca Camera [%s:%ld/%ld]", pConfig->Profile.GetString("/alpaca/host", _("localhost")),
                                           pConfig->Profile.GetLong("/alpaca/port", 6800), deviceNum);
        wxString choice;
        if (FindMatchingChoice(choices, target, &choice))
        {
            pConfig->Profile.SetString("/camera/LastMenuChoice", choice);
            apply_selection_to_control(GEAR_CHOICE_CAMERA, choices, choice);
        }
    }
    else if (type == "TELESCOPE" || type == "MOUNT")
    {
        pConfig->Profile.SetLong("/alpaca/telescope_device", deviceNum);
        wxArrayString choices = Scope::MountList();
        wxString target = wxString::Format(_("Alpaca Mount [%s:%ld/%ld]"), pConfig->Profile.GetString("/alpaca/host", _("localhost")),
                                           pConfig->Profile.GetLong("/alpaca/port", 6800), deviceNum);
        wxString choice;
        if (FindMatchingChoice(choices, target, &choice))
        {
            pConfig->Profile.SetString("/scope/LastMenuChoice", choice);
            apply_selection_to_control(GEAR_CHOICE_SCOPE, choices, choice);
        }
    }
    else
    {
        pConfig->Profile.SetLong("/alpaca/rotator_device", deviceNum);
        wxArrayString choices = Rotator::RotatorList();
        wxString target = wxString::Format("Alpaca Rotator [%s:%ld/%ld]", pConfig->Profile.GetString("/alpaca/host", _("localhost")),
                                           pConfig->Profile.GetLong("/alpaca/port", 6800), deviceNum);
        wxString choice;
        if (FindMatchingChoice(choices, target, &choice))
        {
            pConfig->Profile.SetString("/rotator/LastMenuChoice", choice);
            apply_selection_to_control(GEAR_CHOICE_ROTATOR, choices, choice);
        }
    }

    pConfig->Flush();
    response << jrpc_result(0);
}

static void get_selected_mount(JObj& response, const json_value *params)
{
    response << jrpc_result(pConfig->Profile.GetString("/scope/LastMenuChoice", _("None")));
}

static void get_selected_indi_mount_driver(JObj& response, const json_value *params)
{
    response << jrpc_result(pConfig->Profile.GetString("/indi/INDImount", wxEmptyString));
}

static void set_selected_mount(JObj& response, const json_value *params)
{
    Params p("mount", params);
    const json_value *mount = p.param("mount");
    if (!mount || mount->type != JSON_STRING)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected mount param");
        return;
    }

    if (any_equipment_connected())
    {
        response << jrpc_error(1, "cannot change mount selection while equipment is connected");
        return;
    }

    wxArrayString choices = Scope::MountList();
    wxString choice;
    if (!FindMatchingChoice(choices, mount->string_value, &choice))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "invalid mount selection");
        return;
    }

    pConfig->Profile.SetString("/scope/LastMenuChoice", choice);
    if (choice.Contains("INDI"))
    {
        wxString driverName = extract_bracketed_driver_name(choice);
        if (!driverName.IsEmpty())
            pConfig->Profile.SetString("/indi/INDImount", driverName);
    }
    pConfig->Flush();
    apply_selection_to_control(GEAR_CHOICE_SCOPE, choices, choice);
    response << jrpc_result(0);
}

static void set_selected_indi_mount_driver(JObj& response, const json_value *params)
{
    Params p("mount_driver", "mount", "driver", params);
    const json_value *driver = p.param("mount_driver");
    if (!driver)
        driver = p.param("mount");
    if (!driver)
        driver = p.param("driver");
    if (!driver || driver->type != JSON_STRING)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected mount_driver param");
        return;
    }

    if (any_equipment_connected())
    {
        response << jrpc_error(1, "cannot change mount selection while equipment is connected");
        return;
    }

    pConfig->Profile.SetString("/indi/INDImount", driver->string_value);

    wxString selectedMount = pConfig->Profile.GetString("/scope/LastMenuChoice", _("None"));
    if (selectedMount.Contains("INDI"))
    {
        pConfig->Profile.SetString("/scope/LastMenuChoice", wxString::Format(_("INDI Mount [%s]"), driver->string_value));
        wxArrayString choices = Scope::MountList();
        wxString choice;
        if (FindMatchingChoice(choices, pConfig->Profile.GetString("/scope/LastMenuChoice", _("None")), &choice))
            apply_selection_to_control(GEAR_CHOICE_SCOPE, choices, choice);
    }

    pConfig->Flush();
    response << jrpc_result(0);
}

static void get_selected_camera(JObj& response, const json_value *params)
{
    response << jrpc_result(selected_camera_choice());
}

static void get_selected_camera_id(JObj& response, const json_value *params)
{
    wxString camName = selected_camera_choice();
    wxString key = GearDialog::CameraSelectionKey(camName);
    wxString camId = pConfig->Profile.GetString(key, GuideCamera::DEFAULT_CAMERA_ID);
    response << jrpc_result(camId);
}

static void get_selected_indi_camera_driver(JObj& response, const json_value *params)
{
    response << jrpc_result(pConfig->Profile.GetString("/indi/INDIcam", wxEmptyString));
}

static void set_selected_camera(JObj& response, const json_value *params)
{
    Params p("camera", params);
    const json_value *camera = p.param("camera");
    if (!camera || camera->type != JSON_STRING)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected camera param");
        return;
    }

    if (any_equipment_connected())
    {
        response << jrpc_error(1, "cannot change camera selection while equipment is connected");
        return;
    }

    wxArrayString choices = GuideCamera::GuideCameraList();
    wxString choice;
    if (!FindMatchingChoice(choices, camera->string_value, &choice))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "invalid camera selection");
        return;
    }

    pConfig->Profile.SetString("/camera/LastMenuChoice", choice);
    if (choice.Contains("INDI"))
    {
        wxString driverName = extract_bracketed_driver_name(choice);
        if (!driverName.IsEmpty())
            pConfig->Profile.SetString("/indi/INDIcam", driverName);
    }
    pConfig->Flush();
    apply_selection_to_control(GEAR_CHOICE_CAMERA, choices, choice);
    response << jrpc_result(0);
}

static void set_selected_camera_id(JObj& response, const json_value *params)
{
    Params p("camera_id", params);
    const json_value *cameraId = p.param("camera_id");
    if (!cameraId || cameraId->type != JSON_STRING)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected camera_id param");
        return;
    }

    wxString camName = selected_camera_choice();
    wxString key = GearDialog::CameraSelectionKey(camName);
    pConfig->Profile.SetString(key, cameraId->string_value);
    pConfig->Flush();
    response << jrpc_result(0);
}

static void set_selected_indi_camera_driver(JObj& response, const json_value *params)
{
    Params p("camera_driver", "camera", "driver", params);
    const json_value *driver = p.param("camera_driver");
    if (!driver)
        driver = p.param("camera");
    if (!driver)
        driver = p.param("driver");
    if (!driver || driver->type != JSON_STRING)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected camera_driver param");
        return;
    }

    if (any_equipment_connected())
    {
        response << jrpc_error(1, "cannot change camera selection while equipment is connected");
        return;
    }

    pConfig->Profile.SetString("/indi/INDIcam", driver->string_value);

    wxString selectedCamera = selected_camera_choice();
    if (selectedCamera.Contains("INDI"))
    {
        pConfig->Profile.SetString("/camera/LastMenuChoice", wxString::Format("INDI Camera [%s]", driver->string_value));
        wxArrayString choices = GuideCamera::GuideCameraList();
        wxString choice;
        if (FindMatchingChoice(choices, pConfig->Profile.GetString("/camera/LastMenuChoice", _("None")), &choice))
            apply_selection_to_control(GEAR_CHOICE_CAMERA, choices, choice);
    }

    pConfig->Flush();
    response << jrpc_result(0);
}

static void get_camera_bitdepth(JObj& response, const json_value *params)
{
    int bitDepth = pConfig->Profile.GetInt("/camera/bitdepth", 0);
    if (bitDepth <= 0 && pCamera)
        bitDepth = pCamera->BitsPerPixel();
    response << jrpc_result(bitDepth);
}

static void set_camera_bitdepth(JObj& response, const json_value *params)
{
    Params p("bitdepth", params);
    const json_value *bitdepth = p.param("bitdepth");
    if (!bitdepth || bitdepth->type != JSON_INT)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected bitdepth param");
        return;
    }

    if (bitdepth->int_value < 0 || bitdepth->int_value > 32)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "bitdepth must be in range 0..32");
        return;
    }

    pConfig->Profile.SetInt("/camera/bitdepth", bitdepth->int_value);
    pConfig->Flush();
    response << jrpc_result(0);
}

static void get_selected_aux_mount(JObj& response, const json_value *params)
{
    response << jrpc_result(pConfig->Profile.GetString("/scope/LastAuxMenuChoice", _("None")));
}

static void set_selected_aux_mount(JObj& response, const json_value *params)
{
    Params p("aux_mount", params);
    const json_value *auxMount = p.param("aux_mount");
    if (!auxMount || auxMount->type != JSON_STRING)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected aux_mount param");
        return;
    }

    if (any_equipment_connected())
    {
        response << jrpc_error(1, "cannot change aux mount selection while equipment is connected");
        return;
    }

    wxArrayString choices = Scope::AuxMountList();
    wxString choice;
    if (!FindMatchingChoice(choices, auxMount->string_value, &choice))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "invalid aux mount selection");
        return;
    }

    pConfig->Profile.SetString("/scope/LastAuxMenuChoice", choice);
    if (choice.Contains("INDI"))
    {
        wxString driverName = extract_bracketed_driver_name(choice);
        if (!driverName.IsEmpty())
            pConfig->Profile.SetString("/indi/INDImount", driverName);
    }
    pConfig->Flush();
    apply_selection_to_control(GEAR_CHOICE_AUXSCOPE, choices, choice);
    response << jrpc_result(0);
}

static void get_selected_ao(JObj& response, const json_value *params)
{
    response << jrpc_result(pConfig->Profile.GetString("/stepguider/LastMenuChoice", _("None")));
}

static void set_selected_ao(JObj& response, const json_value *params)
{
    Params p("ao", params);
    const json_value *ao = p.param("ao");
    if (!ao || ao->type != JSON_STRING)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected ao param");
        return;
    }

    if (any_equipment_connected())
    {
        response << jrpc_error(1, "cannot change AO selection while equipment is connected");
        return;
    }

    wxArrayString choices = StepGuider::AOList();
    wxString choice;
    if (!FindMatchingChoice(choices, ao->string_value, &choice))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "invalid AO selection");
        return;
    }

    pConfig->Profile.SetString("/stepguider/LastMenuChoice", choice);
    pConfig->Flush();
    apply_selection_to_control(GEAR_CHOICE_STEPGUIDER, choices, choice);
    response << jrpc_result(0);
}

static void get_selected_rotator(JObj& response, const json_value *params)
{
    response << jrpc_result(pConfig->Profile.GetString("/rotator/LastMenuChoice", _("None")));
}

static void set_selected_rotator(JObj& response, const json_value *params)
{
    Params p("rotator", params);
    const json_value *rotator = p.param("rotator");
    if (!rotator || rotator->type != JSON_STRING)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected rotator param");
        return;
    }

    if (any_equipment_connected())
    {
        response << jrpc_error(1, "cannot change rotator selection while equipment is connected");
        return;
    }

    wxArrayString choices = Rotator::RotatorList();
    wxString choice;
    if (!FindMatchingChoice(choices, rotator->string_value, &choice))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "invalid rotator selection");
        return;
    }

    pConfig->Profile.SetString("/rotator/LastMenuChoice", choice);
    if (choice.Contains("INDI"))
    {
        wxString driverName = extract_bracketed_driver_name(choice);
        if (!driverName.IsEmpty())
            pConfig->Profile.SetString("/indi/INDIrotator", driverName);
    }
    pConfig->Flush();
    apply_selection_to_control(GEAR_CHOICE_ROTATOR, choices, choice);
    response << jrpc_result(0);
}

static bool all_equipment_connected()
{
    Scope *auxMount = pFrame->pGearDialog ? pFrame->pGearDialog->AuxScope() : nullptr;
    StepGuider *ao = TheAO();
    return pCamera && pCamera->Connected &&
        (!pMount || pMount->IsConnected()) &&
        (!pSecondaryMount || pSecondaryMount->IsConnected()) &&
        (!auxMount || auxMount->IsConnected()) &&
        (!ao || ao->IsConnected()) &&
        (!pRotator || pRotator->IsConnected());
}

static void set_profile(JObj& response, const json_value *params)
{
    Params p("id", params);
    const json_value *id = p.param("id");
    if (!id || id->type != JSON_INT)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected profile id param");
        return;
    }

    VERIFY_GUIDER(response);

    wxString errMsg;
    bool error = pFrame->pGearDialog->SetProfile(id->int_value, &errMsg);

    if (error)
    {
        response << jrpc_error(1, errMsg);
    }
    else
    {
        response << jrpc_result(0);
    }
}

static void get_connected(JObj& response, const json_value *params)
{
    response << jrpc_result(all_equipment_connected());
}

static void set_connected(JObj& response, const json_value *params)
{
    Params p("connected", params);
    const json_value *val = p.param("connected");
    if (!val || val->type != JSON_BOOL)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected connected boolean param");
        return;
    }

    VERIFY_GUIDER(response);

    wxString errMsg;
    bool error = val->int_value ? pFrame->pGearDialog->ConnectAll(&errMsg) : pFrame->pGearDialog->DisconnectAll(&errMsg);

    if (error)
    {
        response << jrpc_error(1, errMsg);
    }
    else
    {
        response << jrpc_result(0);
    }
}

static void get_calibrated(JObj& response, const json_value *params)
{
    bool calibrated = pMount && pMount->IsCalibrated() && (!pSecondaryMount || pSecondaryMount->IsCalibrated());
    response << jrpc_result(calibrated);
}

static bool float_param(const json_value *v, double *p)
{
    if (v->type == JSON_INT)
    {
        *p = (double) v->int_value;
        return true;
    }
    else if (v->type == JSON_FLOAT)
    {
        *p = v->float_value;
        return true;
    }

    return false;
}

static bool float_param(const char *name, const json_value *v, double *p)
{
    if (strcmp(name, v->name) != 0)
        return false;

    return float_param(v, p);
}

inline static bool bool_value(const json_value *v)
{
    return v->int_value ? true : false;
}

static bool bool_param(const json_value *jv, bool *val)
{
    if (jv->type != JSON_BOOL && jv->type != JSON_INT)
        return false;
    *val = bool_value(jv);
    return true;
}

static void get_paused(JObj& response, const json_value *params)
{
    VERIFY_GUIDER(response);
    response << jrpc_result(pFrame->pGuider->IsPaused());
}

static void set_paused(JObj& response, const json_value *params)
{
    Params p("paused", "type", params);
    const json_value *jv = p.param("paused");

    bool val;
    if (!jv || !bool_param(jv, &val))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected bool param at index 0");
        return;
    }

    PauseType pause = PAUSE_NONE;

    if (val)
    {
        pause = PAUSE_GUIDING;

        jv = p.param("type");
        if (jv)
        {
            if (jv->type == JSON_STRING)
            {
                if (strcmp(jv->string_value, "full") == 0)
                    pause = PAUSE_FULL;
            }
            else
            {
                response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected string param at index 1");
                return;
            }
        }
    }

    pFrame->SetPaused(pause);

    response << jrpc_result(0);
}

static void loop(JObj& response, const json_value *params)
{
    bool error = pFrame->StartLooping();

    if (error)
        response << jrpc_error(1, "could not start looping");
    else
        response << jrpc_result(0);
}

static void stop_capture(JObj& response, const json_value *params)
{
    pFrame->StopCapturing();
    response << jrpc_result(0);
}

static bool parse_rect(wxRect *r, const json_value *j)
{
    if (j->type != JSON_ARRAY)
        return false;

    int a[4];
    const json_value *jv = j->first_child;
    for (int i = 0; i < 4; i++)
    {
        if (!jv || jv->type != JSON_INT)
            return false;
        a[i] = jv->int_value;
        jv = jv->next_sibling;
    }
    if (jv)
        return false; // extra value

    r->x = a[0];
    r->y = a[1];
    r->width = a[2];
    r->height = a[3];

    return true;
}

static void find_star(JObj& response, const json_value *params)
{
    VERIFY_GUIDER(response);

    Params p("roi", params);

    wxRect roi;
    const json_value *j = p.param("roi");
    if (j && !parse_rect(&roi, j))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "invalid ROI param");
        return;
    }

    bool error = pFrame->AutoSelectStar(roi);

    if (!error)
    {
        const PHD_Point& lockPos = pFrame->pGuider->LockPosition();
        if (lockPos.IsValid())
        {
            response << jrpc_result(lockPos);
            return;
        }
    }

    response << jrpc_error(1, "could not find star");
}

static void get_pixel_scale(JObj& response, const json_value *params)
{
    double scale = pFrame->GetCameraPixelScale();
    if (scale == 1.0)
        response << jrpc_result(NULL_VALUE); // scale unknown
    else
        response << jrpc_result(scale);
}

static void get_app_state(JObj& response, const json_value *params)
{
    EXPOSED_STATE st = Guider::GetExposedState();
    response << jrpc_result(state_name(st));
}

static void get_lock_position(JObj& response, const json_value *params)
{
    VERIFY_GUIDER(response);

    const PHD_Point& lockPos = pFrame->pGuider->LockPosition();
    if (lockPos.IsValid())
        response << jrpc_result(lockPos);
    else
        response << jrpc_result(NULL_VALUE);
}

// {"method": "set_lock_position", "params": [X, Y, true], "id": 1}
static void set_lock_position(JObj& response, const json_value *params)
{
    Params p("x", "y", "exact", params);
    const json_value *p0 = p.param("x"), *p1 = p.param("y");
    double x, y;

    if (!p0 || !p1 || !float_param(p0, &x) || !float_param(p1, &y))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected lock position x, y params");
        return;
    }

    bool exact = true;
    const json_value *p2 = p.param("exact");

    if (p2)
    {
        if (!bool_param(p2, &exact))
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected boolean param at index 2");
            return;
        }
    }

    VERIFY_GUIDER(response);

    bool error;

    if (exact)
        error = pFrame->pGuider->SetLockPosition(PHD_Point(x, y));
    else
        error = pFrame->pGuider->SetLockPosToStarAtPosition(PHD_Point(x, y));

    if (error)
    {
        response << jrpc_error(JSONRPC_INVALID_REQUEST, "could not set lock position");
        return;
    }

    response << jrpc_result(0);
}

inline static const char *string_val(const json_value *j)
{
    return j->type == JSON_STRING ? j->string_value : "";
}

enum WHICH_MOUNT
{
    MOUNT,
    AO,
    WHICH_MOUNT_BOTH,
    WHICH_MOUNT_ERR
};

static WHICH_MOUNT which_mount(const json_value *p)
{
    WHICH_MOUNT r = MOUNT;
    if (p)
    {
        r = WHICH_MOUNT_ERR;
        if (p->type == JSON_STRING)
        {
            if (wxStricmp(p->string_value, "ao") == 0)
                r = AO;
            else if (wxStricmp(p->string_value, "mount") == 0)
                r = MOUNT;
            else if (wxStricmp(p->string_value, "both") == 0)
                r = WHICH_MOUNT_BOTH;
        }
    }
    return r;
}

static void clear_calibration(JObj& response, const json_value *params)
{
    bool clear_mount;
    bool clear_ao;

    if (!params)
    {
        clear_mount = clear_ao = true;
    }
    else
    {
        Params p("which", params);

        clear_mount = clear_ao = false;

        WHICH_MOUNT which = which_mount(p.param("which"));
        switch (which)
        {
        case MOUNT:
            clear_mount = true;
            break;
        case AO:
            clear_ao = true;
            break;
        case WHICH_MOUNT_BOTH:
            clear_mount = clear_ao = true;
            break;
        case WHICH_MOUNT_ERR:
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected param \"mount\", \"ao\", or \"both\"");
            return;
        }
    }

    Mount *mount = TheScope();
    Mount *ao = TheAO();

    if (mount && clear_mount)
        mount->ClearCalibration();

    if (ao && clear_ao)
        ao->ClearCalibration();

    response << jrpc_result(0);
}

static void flip_calibration(JObj& response, const json_value *params)
{
    bool error = pFrame->FlipCalibrationData();

    if (error)
        response << jrpc_error(1, "could not flip calibration");
    else
        response << jrpc_result(0);
}

static void get_lock_shift_enabled(JObj& response, const json_value *params)
{
    VERIFY_GUIDER(response);
    bool enabled = pFrame->pGuider->GetLockPosShiftParams().shiftEnabled;
    response << jrpc_result(enabled);
}

static void set_lock_shift_enabled(JObj& response, const json_value *params)
{
    Params p("enabled", params);
    const json_value *val = p.param("enabled");
    bool enable;
    if (!val || !bool_param(val, &enable))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected enabled boolean param");
        return;
    }

    VERIFY_GUIDER(response);

    pFrame->pGuider->EnableLockPosShift(enable);

    response << jrpc_result(0);
}

static bool is_camera_shift_req(const json_value *params)
{
    Params p("axes", params);
    const json_value *j = p.param("axes");
    if (j)
    {
        const char *axes = string_val(j);
        if (wxStricmp(axes, "x/y") == 0 || wxStricmp(axes, "camera") == 0)
        {
            return true;
        }
    }
    return false;
}

static JObj& operator<<(JObj& j, const LockPosShiftParams& l)
{
    j << NV("enabled", l.shiftEnabled);
    if (l.shiftRate.IsValid())
    {
        j << NV("rate", l.shiftRate) << NV("units", l.shiftUnits == UNIT_ARCSEC ? "arcsec/hr" : "pixels/hr")
          << NV("axes", l.shiftIsMountCoords ? "RA/Dec" : "X/Y");
    }
    return j;
}

static void get_lock_shift_params(JObj& response, const json_value *params)
{
    VERIFY_GUIDER(response);

    const LockPosShiftParams& lockShift = pFrame->pGuider->GetLockPosShiftParams();
    JObj rslt;

    if (is_camera_shift_req(params))
    {
        LockPosShiftParams tmp;
        tmp.shiftEnabled = lockShift.shiftEnabled;
        const ShiftPoint& lock = pFrame->pGuider->LockPosition();
        tmp.shiftRate = lock.ShiftRate() * 3600; // px/sec => px/hr
        tmp.shiftUnits = UNIT_PIXELS;
        tmp.shiftIsMountCoords = false;
        rslt << tmp;
    }
    else
        rslt << lockShift;

    response << jrpc_result(rslt);
}

static bool get_double(double *d, const json_value *j)
{
    if (j->type == JSON_FLOAT)
    {
        *d = j->float_value;
        return true;
    }
    else if (j->type == JSON_INT)
    {
        *d = j->int_value;
        return true;
    }
    return false;
}

static bool parse_point(PHD_Point *pt, const json_value *j)
{
    if (j->type != JSON_ARRAY)
        return false;
    const json_value *jx = j->first_child;
    if (!jx)
        return false;
    const json_value *jy = jx->next_sibling;
    if (!jy || jy->next_sibling)
        return false;
    double x, y;
    if (!get_double(&x, jx) || !get_double(&y, jy))
        return false;
    pt->SetXY(x, y);
    return true;
}

static bool parse_lock_shift_params(LockPosShiftParams *shift, const json_value *params, wxString *error)
{
    // "params":[{"rate":[3.3,1.1],"units":"arcsec/hr","axes":"RA/Dec"}]
    // or
    // "params":{"rate":[3.3,1.1],"units":"arcsec/hr","axes":"RA/Dec"}

    if (params && params->type == JSON_ARRAY)
        params = params->first_child;

    Params p("rate", "units", "axes", params);

    shift->shiftUnits = UNIT_ARCSEC;
    shift->shiftIsMountCoords = true;

    const json_value *j;

    j = p.param("rate");
    if (!j || !parse_point(&shift->shiftRate, j))
    {
        *error = "expected rate value array";
        return false;
    }

    j = p.param("units");
    const char *units = j ? string_val(j) : "";

    if (wxStricmp(units, "arcsec/hr") == 0 || wxStricmp(units, "arc-sec/hr") == 0)
    {
        shift->shiftUnits = UNIT_ARCSEC;
    }
    else if (wxStricmp(units, "pixels/hr") == 0)
    {
        shift->shiftUnits = UNIT_PIXELS;
    }
    else
    {
        *error = "expected units 'arcsec/hr' or 'pixels/hr'";
        return false;
    }

    j = p.param("axes");
    const char *axes = j ? string_val(j) : "";

    if (wxStricmp(axes, "RA/Dec") == 0)
    {
        shift->shiftIsMountCoords = true;
    }
    else if (wxStricmp(axes, "X/Y") == 0)
    {
        shift->shiftIsMountCoords = false;
    }
    else
    {
        *error = "expected axes 'RA/Dec' or 'X/Y'";
        return false;
    }

    return true;
}

static void set_lock_shift_params(JObj& response, const json_value *params)
{
    wxString err;
    LockPosShiftParams shift;
    if (!parse_lock_shift_params(&shift, params, &err))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, err);
        return;
    }

    VERIFY_GUIDER(response);

    pFrame->pGuider->SetLockPosShiftRate(shift.shiftRate, shift.shiftUnits, shift.shiftIsMountCoords, true);

    response << jrpc_result(0);
}

static void save_image(JObj& response, const json_value *params)
{
    VERIFY_GUIDER(response);

    if (!pFrame->pGuider->CurrentImage()->ImageData)
    {
        response << jrpc_error(2, "no image available");
        return;
    }

    wxString fname = wxFileName::CreateTempFileName(MyFrame::GetDefaultFileDir() + PATHSEPSTR + "save_image_");

    if (pFrame->pGuider->SaveCurrentImage(fname))
    {
        ::wxRemove(fname);
        response << jrpc_error(3, "error saving image");
        return;
    }

    JObj rslt;
    rslt << NV("filename", fname);
    response << jrpc_result(rslt);
}

static bool IsValidBinning(GuideCamera *camera, int binning, wxString *message)
{
    auto choices = camera->GetBinningChoices();
    if (choices.find(binning) != choices.end())
        return true;
    wxString buf = wxString::Format("Invalid binning value (%d). Valid choices are: ", binning);
    bool first = true;
    for (auto choice : choices)
    {
        if (first)
            first = false;
        else
            buf.Append(", ");
        buf.Append(wxString::Format("%d", choice.first));
    }
    *message = buf;
    return false;
}

static void capture_single_frame(JObj& response, const json_value *params)
{
    if (pFrame->CaptureActive)
    {
        response << jrpc_error(1, "cannot capture single frame when capture is currently active");
        return;
    }
    if (!pCamera || !pCamera->Connected)
    {
        response << jrpc_error(1, "cannot capture single frame when camera is not connected");
    }

    Params p("exposure", "binning", "gain", "subframe", "path", "save", params);

    const json_value *j;

    int exposure = pFrame->RequestedExposureDuration();
    if ((j = p.param("exposure")) != nullptr)
    {
        if (j->type != JSON_INT || j->int_value < 1 || j->int_value > 10 * 60000)
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected exposure param");
            return;
        }
        exposure = j->int_value;
    }

    wxByte binning = pCamera->GetBinning();
    if ((j = p.param("binning")) != nullptr)
    {
        if (j->type != JSON_INT)
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "binning value must be an integer");
            return;
        }
        wxString message;
        if (!IsValidBinning(pCamera, j->int_value, &message))
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, message);
            return;
        }
        binning = j->int_value;
    }

    int gain = pCamera->GetCameraGain();
    if ((j = p.param("gain")) != nullptr)
    {
        if (j->type != JSON_INT || j->int_value < 0 || j->int_value > 100)
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "invalid gain value: must be between 0 and 100");
            return;
        }
        gain = j->int_value;
    }

    wxRect subframe;
    if ((j = p.param("subframe")) != nullptr)
        if (!parse_rect(&subframe, j))
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "invalid subframe param");
            return;
        }

    wxString path;
    if ((j = p.param("path")) != nullptr)
    {
        if (j->type != JSON_STRING)
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "invalid path param: string expected");
            return;
        }
        wxFileName fn(j->string_value);
        if (!fn.IsAbsolute())
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "path param must be an absolute path");
            return;
        }
        if (fn.Exists())
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "destination file already exists");
            return;
        }
        path = j->string_value;
    }

    bool save = !path.empty();
    if ((j = p.param("save")) != nullptr)
    {
        if (!bool_param(j, &save))
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "save param must be a boolean");
            return;
        }
    }

    if (!save && !path.empty())
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "path param not allowed when save = false");
        return;
    }

    bool err = pFrame->StartSingleExposure(exposure, binning, gain, subframe, save, path);
    if (err)
    {
        response << jrpc_error(2, "failed to start exposure");
        return;
    }

    response << jrpc_result(0);
}

static void get_use_subframes(JObj& response, const json_value *params)
{
    response << jrpc_result(pCamera && pCamera->UseSubframes);
}

static void get_search_region(JObj& response, const json_value *params)
{
    VERIFY_GUIDER(response);
    response << jrpc_result(pFrame->pGuider->GetSearchRegion());
}

struct B64Encode
{
    static const char *const E;
    std::ostringstream os;
    unsigned int t;
    size_t nread;

    B64Encode() : t(0), nread(0) { }
    void append1(unsigned char ch)
    {
        t <<= 8;
        t |= ch;
        if (++nread % 3 == 0)
        {
            os << E[t >> 18] << E[(t >> 12) & 0x3F] << E[(t >> 6) & 0x3F] << E[t & 0x3F];
            t = 0;
        }
    }
    void append(const void *src_, size_t len)
    {
        const unsigned char *src = (const unsigned char *) src_;
        const unsigned char *const end = src + len;
        while (src < end)
            append1(*src++);
    }
    std::string finish()
    {
        switch (nread % 3)
        {
        case 1:
            os << E[t >> 2] << E[(t & 0x3) << 4] << "==";
            break;
        case 2:
            os << E[t >> 10] << E[(t >> 4) & 0x3F] << E[(t & 0xf) << 2] << '=';
            break;
        }
        return os.str();
    }
};
const char *const B64Encode::E = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void get_star_image(JObj& response, const json_value *params)
{
    int reqsize = 15;
    Params p("size", params);
    const json_value *val = p.param("size");
    if (val)
    {
        if (val->type != JSON_INT || (reqsize = val->int_value) < 15)
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "invalid image size param");
            return;
        }
    }

    VERIFY_GUIDER(response);

    Guider *guider = pFrame->pGuider;
    const usImage *img = guider->CurrentImage();
    const PHD_Point& star = guider->CurrentPosition();

    if (guider->GetState() < GUIDER_STATE::STATE_SELECTED || !img->ImageData || !star.IsValid())
    {
        response << jrpc_error(2, "no star selected");
        return;
    }

    int const halfw = wxMin((reqsize - 1) / 2, 31);
    int const fullw = 2 * halfw + 1;
    int const sx = (int) rint(star.X);
    int const sy = (int) rint(star.Y);
    wxRect rect(sx - halfw, sy - halfw, fullw, fullw);
    if (img->Subframe.IsEmpty())
        rect.Intersect(wxRect(img->Size));
    else
        rect.Intersect(img->Subframe);

    B64Encode enc;
    for (int y = rect.GetTop(); y <= rect.GetBottom(); y++)
    {
        const unsigned short *p = img->ImageData + y * img->Size.GetWidth() + rect.GetLeft();
        enc.append(p, rect.GetWidth() * sizeof(unsigned short));
    }

    PHD_Point pos(star);
    pos.X -= rect.GetLeft();
    pos.Y -= rect.GetTop();

    JObj rslt;
    rslt << NV("frame", img->FrameNum) << NV("width", rect.GetWidth()) << NV("height", rect.GetHeight()) << NV("star_pos", pos)
         << NV("pixels", enc.finish());

    response << jrpc_result(rslt);
}

static bool parse_settle(SettleParams *settle, const json_value *j, wxString *error)
{
    bool found_pixels = false, found_time = false, found_timeout = false;

    json_for_each(t, j)
    {
        if (float_param("pixels", t, &settle->tolerancePx))
        {
            found_pixels = true;
            continue;
        }
        double d;
        if (float_param("time", t, &d))
        {
            settle->settleTimeSec = (int) floor(d);
            found_time = true;
            continue;
        }
        if (float_param("timeout", t, &d))
        {
            settle->timeoutSec = (int) floor(d);
            found_timeout = true;
            continue;
        }
    }

    settle->frames = 99999;

    bool ok = found_pixels && found_time && found_timeout;
    if (!ok)
        *error = "invalid settle params";

    return ok;
}

static void guide(JObj& response, const json_value *params)
{
    // params:
    //   settle [object]:
    //     pixels [float]
    //     arcsecs [float]
    //     frames [integer]
    //     time [integer]
    //     timeout [integer]
    //   recalibrate: boolean
    //
    // {"method": "guide", "params": [{"pixels": 0.5, "time": 6, "timeout": 30}, false], "id": 42}
    //    or
    // {"method": "guide", "params": {"settle": {"pixels": 0.5, "time": 6, "timeout": 30}, "recalibrate": false}, "id": 42}
    //
    // todo:
    //   accept tolerance in arcsec or pixels
    //   accept settle time in seconds or frames

    SettleParams settle;

    Params p("settle", "recalibrate", "roi", params);
    const json_value *p0 = p.param("settle");
    if (!p0 || p0->type != JSON_OBJECT)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected settle object param");
        return;
    }
    wxString errMsg;
    if (!parse_settle(&settle, p0, &errMsg))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, errMsg);
        return;
    }

    bool recalibrate = false;
    const json_value *p1 = p.param("recalibrate");
    if (p1)
    {
        if (!bool_param(p1, &recalibrate))
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected bool value for recalibrate");
            return;
        }
    }

    wxRect roi;
    const json_value *p2 = p.param("roi");
    if (p2 && !parse_rect(&roi, p2))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "invalid ROI param");
        return;
    }

    if (recalibrate && !pConfig->Global.GetBoolean("/server/guide_allow_recalibrate", true))
    {
        Debug.AddLine("ignoring client recalibration request since guide_allow_recalibrate = false");
        recalibrate = false;
    }

    wxString err;
    unsigned int ctrlOptions = GUIDEOPT_USE_STICKY_LOCK;
    if (recalibrate)
        ctrlOptions |= GUIDEOPT_FORCE_RECAL;
    if (!PhdController::CanGuide(&err))
        response << jrpc_error(1, err);
    else if (PhdController::Guide(ctrlOptions, settle, roi, &err))
        response << jrpc_result(0);
    else
        response << jrpc_error(1, err);
}

static void dither(JObj& response, const json_value *params)
{
    // params:
    //   amount [integer] - max pixels to move in each axis
    //   raOnly [bool] - when true, only dither ra
    //   settle [object]:
    //     pixels [float]
    //     arcsecs [float]
    //     frames [integer]
    //     time [integer]
    //     timeout [integer]
    //
    // {"method": "dither", "params": [10, false, {"pixels": 1.5, "time": 8, "timeout": 30}], "id": 42}
    //    or
    // {"method": "dither", "params": {"amount": 10, "raOnly": false, "settle": {"pixels": 1.5, "time": 8, "timeout": 30}},
    // "id": 42}

    Params p("amount", "raOnly", "settle", params);
    const json_value *jv;
    double ditherAmt;

    jv = p.param("amount");
    if (!jv || !float_param(jv, &ditherAmt))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected dither amount param");
        return;
    }

    bool raOnly = false;
    jv = p.param("raOnly");
    if (jv)
    {
        if (!bool_param(jv, &raOnly))
        {
            response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected dither raOnly param");
            return;
        }
    }

    SettleParams settle;

    jv = p.param("settle");
    if (!jv || jv->type != JSON_OBJECT)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected settle object param");
        return;
    }
    wxString errMsg;
    if (!parse_settle(&settle, jv, &errMsg))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, errMsg);
        return;
    }

    wxString error;
    if (PhdController::Dither(fabs(ditherAmt), raOnly, settle, &error))
        response << jrpc_result(0);
    else
        response << jrpc_error(1, error);
}

static void shutdown(JObj& response, const json_value *params)
{
    wxGetApp().TerminateApp();

    response << jrpc_result(0);
}

static void get_camera_binning(JObj& response, const json_value *params)
{
    if (pCamera && pCamera->Connected)
    {
        int binning = pCamera->GetBinning();
        response << jrpc_result(binning);
    }
    else
        response << jrpc_error(1, "camera not connected");
}

static void get_camera_frame_size(JObj& response, const json_value *params)
{
    if (pCamera && pCamera->Connected)
    {
        response << jrpc_result(pCamera->FrameSize);
    }
    else
        response << jrpc_error(1, "camera not connected");
}

static void get_guide_output_enabled(JObj& response, const json_value *params)
{
    if (pMount)
        response << jrpc_result(pMount->GetGuidingEnabled());
    else
        response << jrpc_error(1, "mount not defined");
}

static void set_guide_output_enabled(JObj& response, const json_value *params)
{
    Params p("enabled", params);
    const json_value *val = p.param("enabled");
    bool enable;
    if (!val || !bool_param(val, &enable))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected enabled boolean param");
        return;
    }

    if (pMount)
    {
        pMount->SetGuidingEnabled(enable);
        response << jrpc_result(0);
    }
    else
        response << jrpc_error(1, "mount not defined");
}

static bool axis_param(const Params& p, GuideAxis *a)
{
    const json_value *val = p.param("axis");
    if (!val || val->type != JSON_STRING)
        return false;

    bool ok = true;

    if (wxStricmp(val->string_value, "ra") == 0)
        *a = GUIDE_RA;
    else if (wxStricmp(val->string_value, "x") == 0)
        *a = GUIDE_X;
    else if (wxStricmp(val->string_value, "dec") == 0)
        *a = GUIDE_DEC;
    else if (wxStricmp(val->string_value, "y") == 0)
        *a = GUIDE_Y;
    else
        ok = false;

    return ok;
}

static void get_algo_param_names(JObj& response, const json_value *params)
{
    Params p("axis", params);
    GuideAxis a;
    if (!axis_param(p, &a))
    {
        response << jrpc_error(1, "expected axis name param");
        return;
    }
    wxArrayString ary;
    ary.push_back("algorithmName");

    if (pMount)
    {
        GuideAlgorithm *alg = a == GUIDE_X ? pMount->GetXGuideAlgorithm() : pMount->GetYGuideAlgorithm();
        alg->GetParamNames(ary);
    }

    JAry names;
    for (auto it = ary.begin(); it != ary.end(); ++it)
        names << ('"' + json_escape(*it) + '"');

    response << jrpc_result(names);
}

static void get_algo_param(JObj& response, const json_value *params)
{
    Params p("axis", "name", params);
    GuideAxis a;
    if (!axis_param(p, &a))
    {
        response << jrpc_error(1, "expected axis name param");
        return;
    }
    const json_value *name = p.param("name");
    if (!name || name->type != JSON_STRING)
    {
        response << jrpc_error(1, "expected param name param");
        return;
    }
    bool ok = false;
    double val;
    if (pMount)
    {
        GuideAlgorithm *alg = a == GUIDE_X ? pMount->GetXGuideAlgorithm() : pMount->GetYGuideAlgorithm();
        if (strcmp(name->string_value, "algorithmName") == 0)
        {
            response << jrpc_result(alg->GetGuideAlgorithmClassName());
            return;
        }
        ok = alg->GetParam(name->string_value, &val);
    }
    if (ok)
        response << jrpc_result(val);
    else
        response << jrpc_error(1, "could not get param");
}

static void set_algo_param(JObj& response, const json_value *params)
{
    Params p("axis", "name", "value", params);
    GuideAxis a;
    if (!axis_param(p, &a))
    {
        response << jrpc_error(1, "expected axis name param");
        return;
    }
    const json_value *name = p.param("name");
    if (!name || name->type != JSON_STRING)
    {
        response << jrpc_error(1, "expected param name param");
        return;
    }
    const json_value *val = p.param("value");
    double v;
    if (!float_param(val, &v))
    {
        response << jrpc_error(1, "expected param value param");
        return;
    }
    bool ok = false;
    if (pMount)
    {
        GuideAlgorithm *alg = a == GUIDE_X ? pMount->GetXGuideAlgorithm() : pMount->GetYGuideAlgorithm();
        ok = alg->SetParam(name->string_value, v);
    }
    if (ok)
    {
        response << jrpc_result(0);
        if (pFrame->pGraphLog)
            pFrame->pGraphLog->UpdateControls();
    }
    else
        response << jrpc_error(1, "could not set param");
}

static void get_dec_guide_mode(JObj& response, const json_value *params)
{
    Scope *scope = TheScope();
    DEC_GUIDE_MODE mode = scope ? scope->GetDecGuideMode() : DEC_NONE;
    wxString s = Scope::DecGuideModeStr(mode);
    response << jrpc_result(s);
}

static void set_dec_guide_mode(JObj& response, const json_value *params)
{
    Params p("mode", params);
    const json_value *mode = p.param("mode");
    if (!mode || mode->type != JSON_STRING)
    {
        response << jrpc_error(1, "expected mode param");
        return;
    }
    DEC_GUIDE_MODE m = DEC_AUTO;
    bool found = false;
    for (int im = DEC_NONE; im <= DEC_SOUTH; im++)
    {
        m = (DEC_GUIDE_MODE) im;
        if (wxStricmp(mode->string_value, Scope::DecGuideModeStr(m)) == 0)
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        response << jrpc_error(1, "invalid dec guide mode param");
        return;
    }

    Scope *scope = TheScope();
    if (scope)
        scope->SetDecGuideMode(m);

    if (pFrame->pGraphLog)
        pFrame->pGraphLog->UpdateControls();

    response << jrpc_result(0);
}

static void get_settling(JObj& response, const json_value *params)
{
    bool settling = PhdController::IsSettling();
    response << jrpc_result(settling);
}

static void get_variable_delay_settings(JObj& response, const json_value *params)
{
    JObj rslt;

    VarDelayCfg delayParams = pFrame->GetVariableDelayConfig();
    rslt << NV("Enabled", delayParams.enabled) << NV("ShortDelaySeconds", delayParams.shortDelay / 1000)
         << NV("LongDelaySeconds", delayParams.longDelay / 1000);
    response << jrpc_result(rslt);
}

// set_variable_delay values are in units of seconds to match the UI convention in the Advanced Settings dialog
static void set_variable_delay_settings(JObj& response, const json_value *params)
{
    Params p("Enabled", "ShortDelaySeconds", "LongDelaySeconds", params);
    const json_value *p0 = p.param("Enabled");
    const json_value *p1 = p.param("ShortDelaySeconds");
    const json_value *p2 = p.param("LongDelaySeconds");
    bool enabled;
    double shortDelaySec;
    double longDelaySec;
    if (!p0 || !p1 || !p2 || !bool_param(p0, &enabled) || !float_param(p1, &shortDelaySec) || !float_param(p2, &longDelaySec))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected Enabled, ShortDelaySeconds, LongDelaySeconds params)");
        return;
    }
    VarDelayCfg currParams;
    pFrame->SetVariableDelayConfig(enabled, (int) shortDelaySec * 1000, (int) longDelaySec * 1000);
    response << jrpc_result(0);
}

static void get_limit_frame(JObj& response, const json_value *params)
{
    JObj rslt;

    if (!pCamera || !pCamera->HasFrameLimiting || pCamera->LimitFrame.IsEmpty())
        rslt << NV("roi", NULL_VALUE);
    else
        rslt << NV("roi", pCamera->LimitFrame);
    response << jrpc_result(rslt);
}

static void set_limit_frame(JObj& response, const json_value *params)
{
    Params p("roi", params);
    const json_value *j = p.param("roi");
    if (!j)
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "missing required param `roi`");
        return;
    }
    wxRect roi;
    if (j->type != JSON_NULL && !parse_rect(&roi, j))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "invalid ROI param");
        return;
    }
    if (!pCamera)
    {
        response << jrpc_error(1, "no guide camera");
        return;
    }
    if (!pCamera->HasFrameLimiting)
    {
        response << jrpc_error(1, "guide camera does not support frame limiting");
        return;
    }

    VERIFY_GUIDER(response);

    wxString errorMessage;
    bool err = pCamera->SetLimitFrame(roi, 1, &errorMessage);

    if (err)
        response << jrpc_error(1, errorMessage);
    else
        response << jrpc_result(0);
}

static GUIDE_DIRECTION dir_param(const json_value *p)
{
    if (!p || p->type != JSON_STRING)
        return GUIDE_DIRECTION::NONE;

    struct
    {
        const char *s;
        GUIDE_DIRECTION d;
    } dirs[] = {
        { "n", GUIDE_DIRECTION::NORTH },   { "s", GUIDE_DIRECTION::SOUTH },     { "e", GUIDE_DIRECTION::EAST },
        { "w", GUIDE_DIRECTION::WEST },    { "north", GUIDE_DIRECTION::NORTH }, { "south", GUIDE_DIRECTION::SOUTH },
        { "east", GUIDE_DIRECTION::EAST }, { "west", GUIDE_DIRECTION::WEST },   { "up", GUIDE_DIRECTION::UP },
        { "down", GUIDE_DIRECTION::DOWN }, { "left", GUIDE_DIRECTION::LEFT },   { "right", GUIDE_DIRECTION::RIGHT },
    };

    for (unsigned int i = 0; i < WXSIZEOF(dirs); i++)
        if (wxStricmp(p->string_value, dirs[i].s) == 0)
            return dirs[i].d;

    return GUIDE_DIRECTION::NONE;
}

static GUIDE_DIRECTION opposite(GUIDE_DIRECTION d)
{
    switch (d)
    {
    case UP:
        return DOWN;
    case DOWN:
        return UP;
    case LEFT:
        return RIGHT;
    case RIGHT:
        return LEFT;
    default:
        return d;
    }
}

static void guide_pulse(JObj& response, const json_value *params)
{
    Params p("amount", "direction", "which", params);

    const json_value *amount = p.param("amount");
    if (!amount || amount->type != JSON_INT)
    {
        response << jrpc_error(1, "expected amount param");
        return;
    }

    GUIDE_DIRECTION dir = dir_param(p.param("direction"));
    if (dir == GUIDE_DIRECTION::NONE)
    {
        response << jrpc_error(1, "expected direction param");
        return;
    }

    WHICH_MOUNT which = which_mount(p.param("which"));
    Mount *m = nullptr;
    switch (which)
    {
    case MOUNT:
        m = TheScope();
        break;
    case AO:
        m = TheAO();
        break;
    case WHICH_MOUNT_BOTH:
    case WHICH_MOUNT_ERR:
        response << jrpc_error(1, "invalid 'which' param");
        return;
    }

    if (!m || !m->IsConnected())
    {
        response << jrpc_error(1, "device not connected");
        return;
    }

    VERIFY_GUIDER(response);

    if (pFrame->pGuider->IsCalibratingOrGuiding() || m->IsBusy())
    {
        response << jrpc_error(1, "cannot issue guide pulse while calibrating or guiding");
        return;
    }

    int duration = amount->int_value;
    if (duration < 0)
    {
        duration = -duration;
        dir = opposite(dir);
    }

    pFrame->ScheduleManualMove(m, dir, duration);

    response << jrpc_result(0);
}

static const char *parity_str(GuideParity p)
{
    switch (p)
    {
    case GUIDE_PARITY_EVEN:
        return "+";
    case GUIDE_PARITY_ODD:
        return "-";
    default:
        return "?";
    }
}

static void get_calibration_data(JObj& response, const json_value *params)
{
    Params p("which", params);

    WHICH_MOUNT which = which_mount(p.param("which"));
    Mount *m = nullptr;
    switch (which)
    {
    case MOUNT:
        m = TheScope();
        break;
    case AO:
        m = TheAO();
        break;
    case WHICH_MOUNT_BOTH:
    case WHICH_MOUNT_ERR:
    {
        response << jrpc_error(1, "invalid 'which' param");
        return;
    }
    }

    if (!m || !m->IsConnected())
    {
        response << jrpc_error(1, "device not connected");
        return;
    }

    JObj rslt;
    rslt << NV("calibrated", m->IsCalibrated());

    if (m->IsCalibrated())
    {
        rslt << NV("xAngle", degrees(m->xAngle()), 1) << NV("xRate", m->xRate() * 1000.0, 3)
             << NV("xParity", parity_str(m->RAParity())) << NV("yAngle", degrees(m->yAngle()), 1)
             << NV("yRate", m->yRate() * 1000.0, 3) << NV("yParity", parity_str(m->DecParity()))
             << NV("declination", degrees(m->GetCalibrationDeclination()));
    }

    response << jrpc_result(rslt);
}

static void set_cooler_state(JObj& response, const json_value *params)
{
    Params p("enabled", params);
    const json_value *val = p.param("enabled");
    bool enable;
    if (!val || !bool_param(val, &enable))
    {
        response << jrpc_error(JSONRPC_INVALID_PARAMS, "expected enabled boolean param");
        return;
    }

    if (!pCamera || !pCamera->Connected)
    {
        response << jrpc_error(1, "camera not connected");
        return;
    }

    if (!pCamera->HasCooler)
    {
        response << jrpc_error(1, "camera lacks a cooler");
        return;
    }

    if (pCamera->SetCoolerOn(enable))
    {
        response << jrpc_error(1, "failed to set cooler state");
        return;
    }

    if (enable)
    {
        double setpt = pConfig->Profile.GetDouble("/camera/CoolerSetpt", 10.0);
        if (pCamera->SetCoolerSetpoint(setpt))
        {
            response << jrpc_error(1, "failed to set cooler setpoint");
            return;
        }
    }

    response << jrpc_result(0);
}

static void get_cooler_status(JObj& response, const json_value *params)
{
    if (!pCamera || !pCamera->Connected)
    {
        response << jrpc_error(1, "camera not connected");
        return;
    }

    bool on;
    double setpoint, power, temperature;

    bool err = pCamera->GetCoolerStatus(&on, &setpoint, &power, &temperature);
    if (err)
    {
        response << jrpc_error(1, "failed to get cooler status");
        return;
    }

    JObj rslt;

    rslt << NV("coolerOn", on) << NV("temperature", temperature, 1);

    if (on)
    {
        rslt << NV("setpoint", setpoint, 1) << NV("power", power, 1);
    }

    response << jrpc_result(rslt);
}

static void get_sensor_temperature(JObj& response, const json_value *params)
{
    if (!pCamera || !pCamera->Connected)
    {
        response << jrpc_error(1, "camera not connected");
        return;
    }

    double temperature;
    bool err = pCamera->GetSensorTemperature(&temperature);
    if (err)
    {
        response << jrpc_error(1, "failed to get sensor temperature");
        return;
    }

    JObj rslt;
    rslt << NV("temperature", temperature, 1);

    response << jrpc_result(rslt);
}

static void export_config_settings(JObj& response, const json_value *params)
{
    wxString filename(MyFrame::GetDefaultFileDir() + PATHSEPSTR + "phd2_settings.txt");
    bool err = pConfig->SaveAll(filename);

    if (err)
    {
        response << jrpc_error(1, "export settings failed");
        return;
    }

    JObj rslt;
    rslt << NV("filename", filename);

    response << jrpc_result(rslt);
}

struct JRpcCall
{
    wxSocketClient *cli;
    const json_value *req;
    const json_value *method;
    JRpcResponse response;

    JRpcCall(wxSocketClient *cli_, const json_value *req_) : cli(cli_), req(req_), method(nullptr) { }
};

static void dump_request(const JRpcCall& call)
{
    Debug.Write(wxString::Format("evsrv: cli %p request: %s\n", call.cli, json_format(call.req)));
}

static void dump_response(const JRpcCall& call)
{
    wxString s(const_cast<JRpcResponse&>(call.response).str());

    // trim output for huge responses

    // this is very hacky operating directly on the string, but it's not
    // worth bothering to parse and reformat the response
    if (call.method && strcmp(call.method->string_value, "get_star_image") == 0)
    {
        size_t p0, p1;
        if ((p0 = s.find("\"pixels\":\"")) != wxString::npos && (p1 = s.find('"', p0 + 10)) != wxString::npos)
            s.replace(p0 + 10, p1 - (p0 + 10), "...");
    }

    Debug.Write(wxString::Format("evsrv: cli %p response: %s\n", call.cli, s));
}

static bool handle_request(JRpcCall& call)
{
    const json_value *params;
    const json_value *id;

    dump_request(call);

    parse_request(call.req, &call.method, &params, &id);

    if (!call.method)
    {
        call.response << jrpc_error(JSONRPC_INVALID_REQUEST, "invalid request - missing method") << jrpc_id(0);
        return true;
    }

    if (params && !(params->type == JSON_ARRAY || params->type == JSON_OBJECT))
    {
        call.response << jrpc_error(JSONRPC_INVALID_REQUEST, "invalid request - params must be an array or object")
                      << jrpc_id(0);
        return true;
    }

    static struct
    {
        const char *name;
        void (*fn)(JObj& response, const json_value *params);
    } methods[] = {
        { "clear_calibration", &clear_calibration },
        { "deselect_star", &deselect_star },
        { "get_exposure", &get_exposure },
        { "set_exposure", &set_exposure },
        { "get_exposure_durations", &get_exposure_durations },
        { "get_profiles", &get_profiles },
        { "get_profile", &get_profile },
        { "set_profile", &set_profile },
        { "get_connected", &get_connected },
        { "set_connected", &set_connected },
        { "get_calibrated", &get_calibrated },
        { "get_paused", &get_paused },
        { "set_paused", &set_paused },
        { "get_lock_position", &get_lock_position },
        { "set_lock_position", &set_lock_position },
        { "loop", &loop },
        { "stop_capture", &stop_capture },
        { "guide", &guide },
        { "dither", &dither },
        { "find_star", &find_star },
        { "get_pixel_scale", &get_pixel_scale },
        { "get_app_state", &get_app_state },
        { "flip_calibration", &flip_calibration },
        { "get_lock_shift_enabled", &get_lock_shift_enabled },
        { "set_lock_shift_enabled", &set_lock_shift_enabled },
        { "get_lock_shift_params", &get_lock_shift_params },
        { "set_lock_shift_params", &set_lock_shift_params },
        { "save_image", &save_image },
        { "get_star_image", &get_star_image },
        { "get_use_subframes", &get_use_subframes },
        { "get_search_region", &get_search_region },
        { "shutdown", &shutdown },
        { "get_camera_binning", &get_camera_binning },
        { "get_camera_frame_size", &get_camera_frame_size },
        { "get_current_equipment", &get_current_equipment },
        { "get_indi_server", &get_indi_server },
        { "set_indi_server", &set_indi_server },
        { "get_alpaca_server", &get_alpaca_server },
        { "set_alpaca_server", &set_alpaca_server },
        { "discover_alpaca_servers", &discover_alpaca_servers },
        { "query_alpaca_devices", &query_alpaca_devices },
        { "set_selected_alpaca_device", &set_selected_alpaca_device },
        { "get_equipment_choices", &get_equipment_choices },
        { "get_selected_mount", &get_selected_mount },
        { "get_selected_indi_mount_driver", &get_selected_indi_mount_driver },
        { "set_selected_mount", &set_selected_mount },
        { "set_selected_indi_mount_driver", &set_selected_indi_mount_driver },
        { "get_selected_camera", &get_selected_camera },
        { "get_selected_camera_id", &get_selected_camera_id },
        { "get_selected_indi_camera_driver", &get_selected_indi_camera_driver },
        { "set_selected_camera", &set_selected_camera },
        { "set_selected_camera_id", &set_selected_camera_id },
        { "set_selected_indi_camera_driver", &set_selected_indi_camera_driver },
        { "get_camera_bitdepth", &get_camera_bitdepth },
        { "set_camera_bitdepth", &set_camera_bitdepth },
        { "get_selected_aux_mount", &get_selected_aux_mount },
        { "set_selected_aux_mount", &set_selected_aux_mount },
        { "get_selected_ao", &get_selected_ao },
        { "set_selected_ao", &set_selected_ao },
        { "get_selected_rotator", &get_selected_rotator },
        { "set_selected_rotator", &set_selected_rotator },
        { "get_guide_output_enabled", &get_guide_output_enabled },
        { "set_guide_output_enabled", &set_guide_output_enabled },
        { "get_algo_param_names", &get_algo_param_names },
        { "get_algo_param", &get_algo_param },
        { "set_algo_param", &set_algo_param },
        { "get_dec_guide_mode", &get_dec_guide_mode },
        { "set_dec_guide_mode", &set_dec_guide_mode },
        { "get_settling", &get_settling },
        { "guide_pulse", &guide_pulse },
        { "get_calibration_data", &get_calibration_data },
        { "capture_single_frame", &capture_single_frame },
        { "get_cooler_status", &get_cooler_status },
        { "set_cooler_state", &set_cooler_state },
        { "get_ccd_temperature", &get_sensor_temperature },
        { "export_config_settings", &export_config_settings },
        { "get_variable_delay_settings", &get_variable_delay_settings },
        { "set_variable_delay_settings", &set_variable_delay_settings },
        { "get_limit_frame", &get_limit_frame },
        { "set_limit_frame", &set_limit_frame },
    };

    for (unsigned int i = 0; i < WXSIZEOF(methods); i++)
    {
        if (strcmp(call.method->string_value, methods[i].name) == 0)
        {
            (*methods[i].fn)(call.response, params);
            if (id)
            {
                call.response << jrpc_id(id);
                return true;
            }
            else
            {
                return false;
            }
        }
    }

    if (id)
    {
        call.response << jrpc_error(JSONRPC_METHOD_NOT_FOUND, "method not found") << jrpc_id(id);
        return true;
    }
    else
    {
        return false;
    }
}

static void handle_cli_input_complete(wxSocketClient *cli, char *input)
{
    // a dedicated JsonParser instance is used for each line of input since
    // handle_request can recurse if the request causes the event loop to run and we
    // don't want the parser to be reused.
    JsonParser parser;

    if (!parser.Parse(input))
    {
        JRpcCall call(cli, nullptr);
        call.response << jrpc_error(JSONRPC_PARSE_ERROR, parser_error(parser)) << jrpc_id(0);
        dump_response(call);
        do_notify1(cli, call.response);
        return;
    }

    const json_value *root = parser.Root();

    if (root->type == JSON_ARRAY)
    {
        // a batch request

        JAry ary;

        bool found = false;
        json_for_each(req, root)
        {
            JRpcCall call(cli, req);
            if (handle_request(call))
            {
                dump_response(call);
                ary << call.response;
                found = true;
            }
        }

        if (found)
            do_notify1(cli, ary);
    }
    else
    {
        // a single request

        const json_value *const req = root;
        JRpcCall call(cli, req);
        if (handle_request(call))
        {
            dump_response(call);
            do_notify1(cli, call.response);
        }
    }
}

static void handle_cli_input(wxSocketClient *cli)
{
    // Bump refcnt to protect against reentrancy.
    //
    // Some functions like set_connected can cause the event loop to run reentrantly. If the
    // client disconnects before the response is sent and a socket disconnect event is
    // dispatched the client data could be destroyed before we respond.

    ClientDataGuard clidata(cli);

    ClientReadBuf *rdbuf = &clidata->rdbuf;

    wxSocketInputStream sis(*cli);

    while (sis.CanRead())
    {
        if (rdbuf->avail() == 0)
        {
            drain_input(sis);

            JRpcResponse response;
            response << jrpc_error(JSONRPC_INTERNAL_ERROR, "too big") << jrpc_id(0);
            do_notify1(cli, response);

            rdbuf->reset();
            break;
        }
        size_t n = sis.Read(rdbuf->dest, rdbuf->avail()).LastRead();
        if (n == 0)
            break;

        rdbuf->dest += n;

        char *end;
        while ((end = static_cast<char *>(memchr(rdbuf->buf(), '\n', rdbuf->len()))) != nullptr)
        {
            // Move the newline-terminated chunk from the read buffer to a temporary
            // buffer on the stack, and consume the chunk from the read buffer before
            // processing the line. This leaves the read buffer in the correct state to
            // be used again if this function is caller reentrantly.
            char line[ClientReadBuf::SIZE];
            size_t len1 = end - rdbuf->buf();
            memcpy(line, rdbuf->buf(), len1);
            line[len1] = 0;

            char *next = end + 1;
            size_t len2 = rdbuf->dest - next;
            memmove(rdbuf->buf(), next, len2);
            rdbuf->dest = rdbuf->buf() + len2;

            handle_cli_input_complete(cli, line);
        }
    }
}

EventServer::EventServer() : m_configEventDebouncer(nullptr) { }

EventServer::~EventServer() { }

bool EventServer::EventServerStart(unsigned int instanceId)
{
    if (m_serverSocket)
    {
        Debug.AddLine("attempt to start event server when it is already started?");
        return false;
    }

    unsigned int port = 4400 + instanceId - 1;
    wxIPV4address eventServerAddr;
    eventServerAddr.Service(port);
    m_serverSocket = new wxSocketServer(eventServerAddr, wxSOCKET_REUSEADDR);

    if (!m_serverSocket->Ok())
    {
        Debug.Write(wxString::Format("Event server failed to start - Could not listen at port %u\n", port));
        delete m_serverSocket;
        m_serverSocket = nullptr;
        return true;
    }

    m_serverSocket->SetEventHandler(*this, EVENT_SERVER_ID);
    m_serverSocket->SetNotify(wxSOCKET_CONNECTION_FLAG);
    m_serverSocket->Notify(true);

    m_configEventDebouncer = new wxTimer();

    Debug.Write(wxString::Format("event server started, listening on port %u\n", port));

    return false;
}

void EventServer::EventServerStop()
{
    if (!m_serverSocket)
        return;

    for (CliSockSet::const_iterator it = m_eventServerClients.begin(); it != m_eventServerClients.end(); ++it)
    {
        destroy_client(*it);
    }
    m_eventServerClients.clear();

    delete m_serverSocket;
    m_serverSocket = nullptr;

    delete m_configEventDebouncer;
    m_configEventDebouncer = nullptr;

    Debug.AddLine("event server stopped");
}

void EventServer::OnEventServerEvent(wxSocketEvent& event)
{
    wxSocketServer *server = static_cast<wxSocketServer *>(event.GetSocket());

    if (event.GetSocketEvent() != wxSOCKET_CONNECTION)
        return;

    wxSocketClient *client = static_cast<wxSocketClient *>(server->Accept(false));

    if (!client)
        return;

    Debug.Write(wxString::Format("evsrv: cli %p connect\n", client));

    client->SetEventHandler(*this, EVENT_SERVER_CLIENT_ID);
    client->SetNotify(wxSOCKET_LOST_FLAG | wxSOCKET_INPUT_FLAG);
    client->SetFlags(wxSOCKET_NOWAIT);
    client->Notify(true);
    client->SetClientData(new ClientData(client));

    send_catchup_events(client);

    m_eventServerClients.insert(client);
}

void EventServer::OnEventServerClientEvent(wxSocketEvent& event)
{
    wxSocketClient *cli = static_cast<wxSocketClient *>(event.GetSocket());

    if (event.GetSocketEvent() == wxSOCKET_LOST)
    {
        Debug.Write(wxString::Format("evsrv: cli %p disconnect\n", cli));

        unsigned int const n = m_eventServerClients.erase(cli);
        if (n != 1)
            Debug.AddLine("client disconnected but not present in client set!");

        destroy_client(cli);
    }
    else if (event.GetSocketEvent() == wxSOCKET_INPUT)
    {
        handle_cli_input(cli);
    }
    else
    {
        Debug.Write(wxString::Format("unexpected client socket event %d\n", event.GetSocketEvent()));
    }
}

void EventServer::NotifyStartCalibration(const Mount *mount)
{
    SIMPLE_NOTIFY_EV(ev_start_calibration(mount));
}

void EventServer::NotifyCalibrationStep(const CalibrationStepInfo& info)
{
    if (m_eventServerClients.empty())
        return;

    Ev ev("Calibrating");

    ev << NVMount(info.mount) << NV("dir", info.direction) << NV("dist", info.dist) << NV("dx", info.dx) << NV("dy", info.dy)
       << NV("pos", info.pos) << NV("step", info.stepNumber);

    if (!info.msg.empty())
        ev << NV("State", info.msg);

    do_notify(m_eventServerClients, ev);
}

void EventServer::NotifyCalibrationFailed(const Mount *mount, const wxString& msg)
{
    if (m_eventServerClients.empty())
        return;

    Ev ev("CalibrationFailed");
    ev << NVMount(mount) << NV("Reason", msg);

    do_notify(m_eventServerClients, ev);
}

void EventServer::NotifyCalibrationComplete(const Mount *mount)
{
    if (m_eventServerClients.empty())
        return;

    do_notify(m_eventServerClients, ev_calibration_complete(mount));
}

void EventServer::NotifyCalibrationDataFlipped(const Mount *mount)
{
    if (m_eventServerClients.empty())
        return;

    Ev ev("CalibrationDataFlipped");
    ev << NVMount(mount);

    do_notify(m_eventServerClients, ev);
}

void EventServer::NotifyLooping(unsigned int exposure, const Star *star, const FrameDroppedInfo *info)
{
    if (m_eventServerClients.empty())
        return;

    Ev ev("LoopingExposures");
    ev << NV("Frame", exposure);

    double mass = 0., snr, hfd;
    int err = 0;
    wxString status;

    if (star)
    {
        mass = star->Mass;
        snr = star->SNR;
        hfd = star->HFD;
        err = star->GetError();
    }
    else if (info)
    {
        if (Star::WasFound(static_cast<Star::FindResult>(info->starError)))
        {
            mass = info->starMass;
            snr = info->starSNR;
            hfd = info->starHFD;
        }
        err = info->starError;
        status = info->status;
    }

    if (mass)
    {
        ev << NV("StarMass", mass, 0) << NV("SNR", snr, 2) << NV("HFD", hfd, 2);
    }

    if (err)
        ev << NV("ErrorCode", err);

    if (!status.IsEmpty())
        ev << NV("Status", status);

    do_notify(m_eventServerClients, ev);
}

void EventServer::NotifyLoopingStopped()
{
    SIMPLE_NOTIFY("LoopingExposuresStopped");
}

void EventServer::NotifySingleFrameComplete(bool succeeded, const wxString& errorMsg, const SingleExposure& info)
{
    if (m_eventServerClients.empty())
        return;

    Ev ev("SingleFrameComplete");
    ev << NV("Success", succeeded);

    if (!succeeded)
        ev << NV("Error", errorMsg);

    if (info.save)
    {
        ev << NV("Path", info.path);
    }

    do_notify(m_eventServerClients, ev);
}

void EventServer::NotifyStarSelected(const PHD_Point& pt)
{
    SIMPLE_NOTIFY_EV(ev_star_selected(pt));
}

void EventServer::NotifyStarLost(const FrameDroppedInfo& info)
{
    if (m_eventServerClients.empty())
        return;

    Ev ev("StarLost");

    ev << NV("Frame", info.frameNumber) << NV("Time", info.time, 3) << NV("StarMass", info.starMass, 0)
       << NV("SNR", info.starSNR, 2) << NV("HFD", info.starHFD, 2) << NV("AvgDist", info.avgDist, 2);

    if (info.starError)
        ev << NV("ErrorCode", info.starError);

    if (!info.status.IsEmpty())
        ev << NV("Status", info.status);

    do_notify(m_eventServerClients, ev);
}

void EventServer::NotifyGuidingStarted()
{
    SIMPLE_NOTIFY_EV(ev_start_guiding());
}

void EventServer::NotifyGuidingStopped()
{
    SIMPLE_NOTIFY("GuidingStopped");
}

void EventServer::NotifyPaused()
{
    SIMPLE_NOTIFY_EV(ev_paused());
}

void EventServer::NotifyResumed()
{
    SIMPLE_NOTIFY("Resumed");
}

void EventServer::NotifyGuideStep(const GuideStepInfo& step)
{
    if (m_eventServerClients.empty())
        return;

    Ev ev("GuideStep");

    ev << NV("Frame", step.frameNumber) << NV("Time", step.time, 3) << NVMount(step.mount) << NV("dx", step.cameraOffset.X, 3)
       << NV("dy", step.cameraOffset.Y, 3) << NV("RADistanceRaw", step.mountOffset.X, 3)
       << NV("DECDistanceRaw", step.mountOffset.Y, 3) << NV("RADistanceGuide", step.guideDistanceRA, 3)
       << NV("DECDistanceGuide", step.guideDistanceDec, 3);

    if (step.durationRA > 0)
    {
        ev << NV("RADuration", step.durationRA)
           << NV("RADirection", step.mount->DirectionStr((GUIDE_DIRECTION) step.directionRA));
    }

    if (step.durationDec > 0)
    {
        ev << NV("DECDuration", step.durationDec)
           << NV("DECDirection", step.mount->DirectionStr((GUIDE_DIRECTION) step.directionDec));
    }

    if (step.mount->IsStepGuider())
    {
        ev << NV("Pos", step.aoPos);
    }

    ev << NV("StarMass", step.starMass, 0) << NV("SNR", step.starSNR, 2) << NV("HFD", step.starHFD, 2)
       << NV("AvgDist", step.avgDist, 2);

    if (step.starError)
        ev << NV("ErrorCode", step.starError);

    if (step.raLimited)
        ev << NV("RALimited", true);

    if (step.decLimited)
        ev << NV("DecLimited", true);

    do_notify(m_eventServerClients, ev);
}

void EventServer::NotifyGuidingDithered(double dx, double dy)
{
    if (m_eventServerClients.empty())
        return;

    Ev ev("GuidingDithered");
    ev << NV("dx", dx, 3) << NV("dy", dy, 3);

    do_notify(m_eventServerClients, ev);
}

void EventServer::NotifySetLockPosition(const PHD_Point& xy)
{
    if (m_eventServerClients.empty())
        return;

    do_notify(m_eventServerClients, ev_set_lock_position(xy));
}

void EventServer::NotifyLockPositionLost()
{
    SIMPLE_NOTIFY("LockPositionLost");
}

void EventServer::NotifyLockShiftLimitReached()
{
    SIMPLE_NOTIFY("LockPositionShiftLimitReached");
}

void EventServer::NotifyAppState()
{
    if (m_eventServerClients.empty())
        return;

    do_notify(m_eventServerClients, ev_app_state());
}

void EventServer::NotifySettleBegin()
{
    SIMPLE_NOTIFY("SettleBegin");
}

void EventServer::NotifySettling(double distance, double time, double settleTime, bool starLocked)
{
    if (m_eventServerClients.empty())
        return;

    Ev ev(ev_settling(distance, time, settleTime, starLocked));

    Debug.Write(wxString::Format("evsrv: %s\n", ev.str()));

    do_notify(m_eventServerClients, ev);
}

void EventServer::NotifySettleDone(const wxString& errorMsg, int settleFrames, int droppedFrames)
{
    if (m_eventServerClients.empty())
        return;

    Ev ev(ev_settle_done(errorMsg, settleFrames, droppedFrames));

    Debug.Write(wxString::Format("evsrv: %s\n", ev.str()));

    do_notify(m_eventServerClients, ev);
}

void EventServer::NotifyAlert(const wxString& msg, int type)
{
    if (m_eventServerClients.empty())
        return;

    Ev ev("Alert");
    ev << NV("Msg", msg);

    wxString s;
    switch (type)
    {
    case wxICON_NONE:
    case wxICON_INFORMATION:
    default:
        s = "info";
        break;
    case wxICON_QUESTION:
        s = "question";
        break;
    case wxICON_WARNING:
        s = "warning";
        break;
    case wxICON_ERROR:
        s = "error";
        break;
    }
    ev << NV("Type", s);

    do_notify(m_eventServerClients, ev);
}

template<typename T>
static void NotifyGuidingParam(const EventServer::CliSockSet& clients, const wxString& name, T val)
{
    if (clients.empty())
        return;

    Ev ev("GuideParamChange");
    ev << NV("Name", name);
    ev << NV("Value", val);

    do_notify(clients, ev);
}

void EventServer::NotifyGuidingParam(const wxString& name, double val)
{
    ::NotifyGuidingParam(m_eventServerClients, name, val);
}

void EventServer::NotifyGuidingParam(const wxString& name, int val)
{
    ::NotifyGuidingParam(m_eventServerClients, name, val);
}

void EventServer::NotifyGuidingParam(const wxString& name, bool val)
{
    ::NotifyGuidingParam(m_eventServerClients, name, val);
}

void EventServer::NotifyGuidingParam(const wxString& name, const wxString& val)
{
    ::NotifyGuidingParam(m_eventServerClients, name, val);
}

void EventServer::NotifyConfigurationChange()
{
    if (m_configEventDebouncer == nullptr || m_configEventDebouncer->IsRunning())
        return;

    Ev ev("ConfigurationChange");
    do_notify(m_eventServerClients, ev);
    m_configEventDebouncer->StartOnce(0);
}
