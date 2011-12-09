/**
 * @file
 * DaemonTCPTransport is an implementation of TCPTransportBase for daemons.
 */

/******************************************************************************
 * Copyright 2009-2011, Qualcomm Innovation Center, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 ******************************************************************************/

#include <qcc/platform.h>
#include <qcc/IPAddress.h>
#include <qcc/Socket.h>
#include <qcc/SocketStream.h>
#include <qcc/String.h>
#include <qcc/StringUtil.h>
#include <qcc/IfConfig.h>

#include <alljoyn/BusAttachment.h>
#include <alljoyn/TransportMask.h>
#include <alljoyn/Session.h>

#include "BusInternal.h"
#include "RemoteEndpoint.h"
#include "Router.h"
#include "ConfigDB.h"
#include "NameService.h"
#include "DaemonTCPTransport.h"

/*
 * How the transport fits into the system
 * ======================================
 *
 * AllJoyn provides the concept of a Transport which provides a relatively
 * abstract way for the daemon to use different network mechanisms for getting
 * Messages from place to another.  Conceptually, think of, for example, a Unix
 * transport that moves bits using unix domain sockets, a Bluetooth transport
 * that moves bits over a Bluetooth link and a TCP transport that moves Messages
 * over a TCP connection.
 *
 * In networking 101, one discovers that BSD sockets is oriented toward clients
 * and servers.  There are different sockets calls required for a program
 * implementing a server-side part and a client side part.  The server-side
 * listens for incoming connection requests and the client-side initiates the
 * requests.  AllJoyn clients are bus attachments that our Applications may use
 * and these can only initiate connection requests to AllJoyn daemons.  Although
 * dameons may at first blush appear as the service side of a typical BSD
 * sockets client-server pair, it turns out that while daemons obviously must
 * listen for incoming connections, they also must be able to initiate
 * connection requests to other daemons.  It turns out that there is very little
 * in the way of common code when comparing the client version of a TCP
 * transport and a daemon version.  Therefore you will find a DaemonTCPTransport
 * class in here the dameon directory and a client version, called simply
 * TCPTransport, in the src directory.
 *
 * This file is the DaemonTCPTransport.  It needs to act as both a client and a
 * server explains the presence of both connect-like methods and listen-like
 * methods here.
 *
 * A fundamental idiom in the AllJoyn system is that of a thread.  Active
 * objects in the system that have threads wandering through them will implement
 * Start(), Stop() and Join() methods.  These methods work together to manage
 * the autonomous activities that can happen in a DaemonTCPTransport.  These
 * activities are carried out by so-called hardware threads.  POSIX defines
 * functions used to control hardware threads, which it calls pthreads.  Many
 * threading packages use similar constructs.
 *
 * In a threading package, a start method asks the underlying system to arrange
 * for the start of thread execution.  Threads are not necessarily running when
 * the start method returns, but they are being *started*.  Some time later, a
 * thread of execution appears in a thread run function, at which point the
 * thread is considered *running*.  In the case of the DaemonTCPTransport, the Start() method
 * spins up a thread to run the BSD sockets' server accept loop.  This
 * also means that as soon as Start() is executed, a thread may be using underlying
 * socket file descriptors and one must be very careful about convincing the
 * accept loop thread to exit before deleting the resources.
 *
 * In generic threads packages, executing a stop method asks the underlying
 * system to arrange for a thread to end its execution.  The system typically
 * sends a message to the thread to ask it to stop doing what it is doing.  The
 * thread is running until it responds to the stop message, at which time the
 * run method exits and the thread is considered *stopping*.  The
 * DaemonTCPTransport provides a Stop() method to do exactly that.
 *
 * Note that neither of Start() nor Stop() are synchronous in the sense that one
 * has actually accomplished the desired effect upon the return from a call.  Of
 * particular interest is the fact that after a call to Stop(), threads will
 * still be *running* for some non-deterministic time.
 *
 * In order to wait until all of the threads have actually stopped, a blocking
 * call is required.  In threading packages this is typically called join, and
 * our corresponding method is called Join().  A user of the DaemonTcpTransport
 * must assume that immediately after a call to Start() is begun, and until a
 * call to Join() returns, there may be threads of execution wandering anywhere
 * in the DaemonTcpTransport and in any callback registered by the caller.
 *
 * Internals
 * =========
 *
 * We spend a lot of time on the threading aspects of the transport since they
 * are often the hardest part to get right and are complicated.  This is where
 * the bugs live.
 *
 * As mentioned above, the AllJoyn system uses the concept of a Transport.  You
 * are looking at the DaemonTCPTransport.  Each transport also has the concept
 * of an Endpoint.  The most important function fo an endpoint is to provide
 * non-blocking semantics to higher level code.  This is provided by a transmit
 * thread on the write side which can block without blocking the higher level
 * code, and a receive thread which can similarly block waiting for data without
 * blocking the higher level code.
 *
 * Endpoints are specialized into the LocalEndpoint and the RemoteEndpoint
 * classes.  LocalEndpoint represents a connection from a router to the local
 * bus attachment or daemon (within the "current" process).  A RemoteEndpoint
 * represents a connection from a router to a remote attachment or daemon.  By
 * definition, the DaemonTCPTransport provides RemoteEndpoint functionality.
 *
 * RemoteEndpoints are further specialized according to the flavor of the
 * corresponding transport, and so you will see a DaemonTCPEndpoint class
 * defined below which provides functionality to send messages from the local
 * router to a destination off of the local process using a TCP transport
 * mechanism.
 *
 * RemoteEndpoints use AllJoyn stream objects to actually move bits.  This
 * is a thin layer on top of a Socket (which is another thin layer on top of
 * a BSD socket) that provides PushBytes() adn PullBytes() methods.  Remote
 * endpoints also provide the transmit thread and receive threads mentioned
 * above.
 *
 * The job of the receive thread is to loop waiting for bytes to appear on the
 * input side of the stream and to unmarshal them into AllJoyn Messages.  Once
 * an endpoint has a message, it calls into the Message router (PushMessage) to
 * arrange for delivery.  The job of the transmit thread is to loop waiting for
 * Messages to appear on its transmit queue.  When a Message is put on the queue
 * by a Message router, the transmit thread will pull it off and marshal it,
 * then it will write the bytes to the transport mechanism.
 *
 * The DaemonTCPEndpoint inherits the infrastructure requred to do most of its
 * work from the more generic RemoteEndpoint class.  It needs to do specific
 * TCP-related work and also provide for authenticating the endpoint before it
 * is allowed to start pumping messages.  Authentication means running some
 * mysterious (to us) process that may involve some unknown number of challenge
 * and response messsages being exchanged between the client and server side of
 * the connection.  Since we cannot block a caller waiting for authentication,
 * this must done on another thread; and this must be done before the
 * RemoteEndpoint is Start()ed -- before its transmit and receive threads are
 * started, lest they start pumping messages and interfering with the
 * authentication process.
 *
 * Authentication can, of course, succeed or fail based on timely interaction
 * between the two sides, but it can also be abused in a denial of service
 * attack.  If a client simply starts the process but never responds, it could
 * tie up a daemon's resources, and coordinated action could bring down a
 * daemon.  Because of this, we need to provide a way to reach in and abort
 * authentications that are "taking too long."
 *
 * As described above, a daemon can listen for inbound connections and it can
 * initiate connections to remote daemons.  Authentication must happen in both
 * cases.
 *
 * If you consider all that is happening, we are talking about a complicated
 * system of many threads that are appearing and disappearing in the system at
 * unpredictable times.  These threads have dependencies in the resources
 * associated with them (sockets and events in particular).  These resources may
 * have further dependencies that must be respected.  For example, Events may
 * have references to Sockets.  The Sockets must not be released before the
 * Events are released, because the events would be left with stale handles.  An
 * even scarier case is if an underlying Socket FD is reused at just the wrong
 * time, it would be possible to switch a Socket FD from one connection to
 * another out from under an Event without its knowledge.
 *
 * To summarize, consider the following "big picture' view of the transport.  A
 * single DaemonTCPTransport is constructed if the daemon TransportList
 * indicates that TCP support is required.  The high-level daemon code (see
 * bbdaemon.cc for example) builds a TransportFactoryContainer that is
 * initialized with a factory that knows how to make DaemonTCPTransport objects
 * if they are needed, and associates the factory with the string "tcp".  The
 * daemon also constructs "server args" which may contain the string "tcp" or
 * "bluetooth" or "unix".  If the factory container provides a "tcp" factory and
 * the server args specify a "tcp" transport is needed then a DaemonTCPTransport
 * object is instantiated and entered into the daemon's internal transport list
 * (list of available transports).  Also provided for each transport is an abstract
 * address to listen for incoming connection requests on.
 *
 * When the daemon is brought up, its TransportList is Start()ed.  The transport
 * specs string (e.g., "unix:abstract=alljoyn;tcp:;bluetooth:") is provided to
 * TransportList::Start() as a parameter.  The transport specs string is parsed
 * and in the example above, results in "unix" transports, "tcp" transports and
 * "bluetooth" transports being instantiated and started.  As mentioned
 * previously "tcp" in the daemon translates into DaemonTCPTransport.  Once the
 * desired transports are instantiated, each is Start()ed in turn.  In the case
 * of the DaemonTCPTransport, this will start the server accept loop.  Initially
 * there are no sockets to listen on.
 *
 * The daemon then needs to start listening on some inbound addresses and ports.
 * This is done by the StartListen() command which you can find in bbdaemon, for
 * example.  This alwo takes the same king of server args string shown above but
 * this time the address and port information are used.  For example, one might
 * use the string "tcp:addr=0.0.0.0,port=9955;" to specify which address and
 * port to listen to.  This Bus::StartListen() call is translated into a
 * DaemonTCPTransport::StartListen() call which is provided with the string
 * which we call a "listen spec".  Our StartListen() will create a Socket, bind
 * the socket to the address and port provided and save the new socket on a list
 * of "listenFds." It will then Alert() the already running server accept loop
 * thread -- see DaemonTCPTransport::Run().  Each time through the server accept
 * loop, Run() will examine the list of listenFds and will associate an Event
 * with the corresponding socketFd and wait for connection requests.
 *
 * There is a complementary call to stop listening on addresses.  Since the
 * server accept loop is depending on the associated sockets, StopListen must
 * not close those Sockets, it must ask the server accept loop to do so in a
 * coordinated way.
 *
 * When an inbound connection request is received, the accept loop will wake up
 * and create a DaemonTCPEndpoint for the *proposed* new connection.  Recall
 * that an endpoint is not brought up immediately, but an authentication step
 * must be performed.  The server accept loop starts this process by placing the
 * new DaemonTCPEndpoint on an authList, or list of authenticating endpoints.
 * It then calls the endpoint Authenticate() method which spins up an
 * authentication thread and returns immediately.  This process transfers the
 * responsibility for the connection and its resources to the authentication
 * thread.  Authentication can succeed, fail, or take to long and be aborted.
 *
 * If authentication succeeds, the authentication thread calls back into the
 * DaemonTCPTransport's Authenticated() method.  Along with indicating that
 * authentication has completed successfully, this transfers ownership of the
 * DaemonTCPEndpoint back to the DaemonTCPTransport from the authentication
 * thread.  At this time, the DaemonTCPEndpoint is Start()ed which spins up
 * the transmit and receive threads and enables Message routing across the
 * transport.
 *
 * If the authentication fails, the authentication thread simply sets a the
 * DaemonTCPEndpoint state to FAILED and exits.  The server accept loop looks at
 * authenticating endpoints (those on the authList)each time through its loop.
 * If an endpoint has failed authentication, and its thread has actually gone
 * away (or more precisely is at least going away in such a way that it will
 * never touch the endpoing data structure again).  This means that the endpoint
 * can be deleted.
 *
 * If the authentication takes "too long" we assume that a denial of service
 * attack in in progress.  We call Abort() on such an endpoint which will most
 * likely induce a failure (unless we happen to call abort just as the endpoint
 * actually finishes the authentication which is highly unlikely but okay).
 * This Abort() will cause the endpoint to be scavenged using the above mechanism
 * the next time through the accept loop.
 *
 * A daemon transport can accept incoming connections, and it can make outgoing
 * connections to another daemon.  This case is simpler than the accept case
 * since it is expected that a socket connect can block, so it is possible to do
 * authentication in the context of the thread calling Connect().  Connect() is
 * provided a so-called "connect spec" which provides an IP address ("addr=xxxx"),
 * port ("port=yyyy") and address family ("family=zzzz") in a String.
 *
 * A check is always made to catch an attempt for the daemon to connect to
 * itself which is a system-defined error (it causes the daemon grief, so we
 * avoid it here by looking to see if one of the listenFds is listening on an
 * interface that corresponds to the address in the connect spec).
 *
 * If the connect is allowed, we do the usual BSD sockets thing where we create
 * a socket and connect to the specified remote address.  The DBus spec says that
 * all connections must begin with one uninterpreted byte so we send that.  This
 * byte is only meaningful in Unix domain sockets transports, but we must send it
 * anyway.
 *
 * The next step is to create a DaemonTCPEndpoint and to put it on the endpointList.
 * Note that the endpoint doesn't go on the authList as in the server case, it
 * goes on the list of active endpoints.  This is because a failure to authenticate
 * on the client side results in a call to EndpointExit which is the same code path as
 * a failure when the endpoint is up.  The failing endpoint must be on the endpoint
 * list in order to allow authentication errors to be propagated back to higher-level
 * code in a meaningful context.  Once the endpoint is stored on the list, Connect()
 * starts client-side Authentication with the remote (server) side.  If Authentication
 * succeeds, the endpoint is Start()ed which will spin up the rx and tx threads that
 * start Message routing across the link.  The endpoint is left on the endpoint list
 * in this case.  If authentication fails, the endpoint is removed from the active
 * list.  This is thread-safe since there is no authentication thread running because
 * the authentication was done in the context of the thread calling Connect() which
 * is the one deleting the endpoint; and no rx or tx thread is spun up if the
 * authentication fails.
 *
 * Shutting the DaemonTCPTransport down involves orchestrating the orderly termination
 * of:
 *
 *   1) Threads that may be running in the server accept loop with associated Events
 *      and their dependent socketFds stored in the listenFds list.
 *   2) Threads that may be running authentication with associated endpoint objects,
 *      streams and SocketFds.  These threads are accessible through endpoint objects
 *      stored on the authList.
 *   3) Threads that may be running the rx and tx loops in endpoints which are up and
 *      running, transporting routable Messages through the system.
 *
 * Note that we also have to understand and deal with the fact that threads
 * running in state (2) above, will exit and depend on the server accept loop to
 * scavenge the associated objects off of the authList and delete them.  This
 * means that the server accept loop cannot be Stop()ped until the authList is
 * empty.  We further have to understand that threads running in state (3) above
 * will depend on the hooked EndpointExit function to dispose of associated
 * resources.  This will happen in the context of either the transmit or receive
 * thread (the last to go).  We can't delete the transport until all of its
 * associated endpoint threads are Join()ed.  Also, since the server accept loop
 * is looking at the list of listenFDs, we must be careful about deleting those
 * sockets out from under the server thread.  The system should call
 * StopListen() on all of the listen specs it called StartListen() on; but we
 * need to be prepared to clean up any "unstopped" listen specs in a coordinated
 * way.  This, in turn, means that the server accept loop cannot be Stop()ped
 * until all of the listenFds are cleaned up.
 *
 * There are a lot of dependencies here, so be careful when making changes to
 * the thread and resource management here.  It's quite easy to shoot yourself
 * in multiple feet you never knew you had if you make an unwise modification,
 * and sometimes the results are tiny little time-bombs set to go off in
 * completely unrelated code (if, for example, a socket is deleted and reused
 * by another piece of code while the transport still has an event referencing
 * the socket now used by the other module).
 */

#define QCC_MODULE "ALLJOYN_DAEMON_TCP"

using namespace std;
using namespace qcc;

const uint32_t TCP_LINK_TIMEOUT_PROBE_ATTEMPTS       = 1;
const uint32_t TCP_LINK_TIMEOUT_PROBE_RESPONSE_DELAY = 10;
const uint32_t TCP_LINK_TIMEOUT_MIN_LINK_TIMEOUT     = 40;

namespace ajn {

/*
 * An endpoint class to handle the details of authenticating a connection in a
 * way that avoids denial of service attacks.
 */
class DaemonTCPEndpoint : public RemoteEndpoint {
  public:
    enum AuthState {
        ILLEGAL = 0,
        INITIALIZED,
        AUTHENTICATING,
        FAILED,
        AVORTED,
        SUCCEEDED
    };

    DaemonTCPEndpoint(DaemonTCPTransport* transport,
                      BusAttachment& bus,
                      bool incoming,
                      const qcc::String connectSpec,
                      qcc::SocketFd sock,
                      const qcc::IPAddress& ipAddr,
                      uint16_t port)
        : RemoteEndpoint(bus, incoming, connectSpec, m_stream, "tcp"),
        m_transport(transport),
        m_state(INITIALIZED),
        m_tStart(qcc::Timespec(0)),
        m_authThread(this, transport),
        m_stream(sock),
        m_ipAddr(ipAddr),
        m_port(port),
        m_wasSuddenDisconnect(!incoming) { }

    virtual ~DaemonTCPEndpoint() { }

    void SetStartTime(qcc::Timespec tStart) { m_tStart = tStart; }
    qcc::Timespec GetStartTime(void) { return m_tStart; }
    QStatus Authenticate(void);
    void Abort(void);
    const qcc::IPAddress& GetIPAddress() { return m_ipAddr; }
    uint16_t GetPort() { return m_port; }
    bool IsFailed(void) { return m_state == FAILED; }
    bool IsSuddenDisconnect() { return m_wasSuddenDisconnect; }
    void SetSuddenDisconnect(bool val) { m_wasSuddenDisconnect = val; }

    QStatus SetLinkTimeout(uint32_t& linkTimeout)
    {
        QStatus status = ER_OK;
        if (linkTimeout > 0) {
            uint32_t to = max(linkTimeout, TCP_LINK_TIMEOUT_MIN_LINK_TIMEOUT);
            to -= TCP_LINK_TIMEOUT_PROBE_RESPONSE_DELAY * TCP_LINK_TIMEOUT_PROBE_ATTEMPTS;
            status = RemoteEndpoint::SetLinkTimeout(to, TCP_LINK_TIMEOUT_PROBE_RESPONSE_DELAY, TCP_LINK_TIMEOUT_PROBE_ATTEMPTS);
            if ((status == ER_OK) && (to > 0)) {
                linkTimeout = to + TCP_LINK_TIMEOUT_PROBE_RESPONSE_DELAY * TCP_LINK_TIMEOUT_PROBE_ATTEMPTS;
            }

        } else {
            RemoteEndpoint::SetLinkTimeout(0, 0, 0);
        }
        return status;
    }

    /*
     * Return true if the auth thread is STARTED, RUNNING or STOPPING.  A true
     * response means the authentication thread is in a state that indicates
     * a possibility it might touch the endpoint data structure.  This means
     * don't delete the endpoing if this method returns true.  This method
     * indicates nothing about endpoint rx and tx thread state.
     */
    bool IsAuthThreadRunning(void)
    {
        return m_authThread.IsRunning();
    }

  private:
    class AuthThread : public qcc::Thread {
      public:
        AuthThread(DaemonTCPEndpoint* conn, DaemonTCPTransport* trans) : Thread("auth"), m_transport(trans) { }
      private:
        virtual qcc::ThreadReturn STDCALL Run(void* arg);

        DaemonTCPTransport* m_transport;
    };

    DaemonTCPTransport* m_transport;  /**< The server holding the connection */
    volatile AuthState m_state;       /**< The state of the endpoint authentication process */
    qcc::Timespec m_tStart;           /**< Timestamp indicating when the authentication process started */
    AuthThread m_authThread;          /**< Thread used to do blocking calls during startup */
    qcc::SocketStream m_stream;       /**< Stream used by authentication code */
    qcc::IPAddress m_ipAddr;          /**< Remote IP address. */
    uint16_t m_port;                  /**< Remote port. */
    bool m_wasSuddenDisconnect;       /**< If true, assumption is that any disconnect is unexpected due to lower level error */
};

QStatus DaemonTCPEndpoint::Authenticate(void)
{
    QCC_DbgTrace(("DaemonTCPEndpoint::Authenticate()"));
    /*
     * Start the authentication thread.
     */
    QStatus status = m_authThread.Start(this);
    if (status != ER_OK) {
        m_state = FAILED;
    }
    return status;
}

void DaemonTCPEndpoint::Abort(void)
{
    QCC_DbgTrace(("DaemonTCPEndpoint::Abort()"));
    m_authThread.Stop();
}

void* DaemonTCPEndpoint::AuthThread::Run(void* arg)
{
    QCC_DbgTrace(("DaemonTCPEndpoint::AuthThread::Run()"));

    DaemonTCPEndpoint* conn = reinterpret_cast<DaemonTCPEndpoint*>(arg);

    conn->m_state = AUTHENTICATING;

    /*
     * We're running an authentication process here and we are cooperating
     * with the main server thread.  This thread is running in an object
     * that is allocated on the heap, and the server is managing these
     * objects so we need to coordinate getting all of this cleaned up.
     *
     * There is a state variable that only we write.  The server thread only
     * reads this variable, so there are no data sharing issues.  If there is
     * an authentication failure, this thread sets that state variable to
     * FAILED and then exits.  The server holds a list of currently
     * authenticating connections and will look for FAILED connections when it
     * runs its Accept loop.  If it finds one, it will then delete the
     * connection which will cause a Join() to this thread.  Since we set FAILED
     * immediately before exiting, there will be no problem having the server
     * block waiting for the Join() to complete.  We fail authentication here
     * and let the server clean up after us, lazily.
     *
     * If we succeed in the authentication process, we set the state variable
     * to SUCEEDED and then call back into the server telling it that we are
     * up and running.  It needs to take us off of the list of authenticating
     * connections and put us on the list of running connections.  This thread
     * will quickly go away and will be replaced by the Rx- and TxThreads of
     * the running RemoteEndpoint.
     *
     * If we are running an authentication process, we are probably ultimately
     * blocked on a socket.  We expect that if the server is asked to shut
     * down, it will run through its list of authenticating connections and
     * Stop() each one.  That will cause a thread Stop() which should unblock
     * all of the reads and return an error which will eventually pop out here
     * with an authentication failure.
     *
     * Finally, if the server decides we've spent too much time here and we are
     * actually a denial of service attack, it can close us down by doing an
     * Abort() on the endpoing, which will do a thread Stop() which will pop out
     * of here as an authentication failure as well.  The only ways out of this
     * method must be with state = FAILED or state = SUCCEEDED.
     */
    uint8_t byte;
    size_t nbytes;

    /*
     * Eat the first byte of the stream.  This is required to be zero by the
     * DBus protocol.  It is used in the Unix socket implementation to carry
     * out-of-band capabilities, but is discarded here.  We do this here since
     * it involves a read that can block.
     */
    QStatus status = conn->m_stream.PullBytes(&byte, 1, nbytes);
    if ((status != ER_OK) || (nbytes != 1) || (byte != 0)) {
        conn->m_stream.Close();
        conn->m_state = FAILED;
        QCC_LogError(status, ("Failed to read first byte from stream"));
        return (void*)ER_FAIL;
    }

    /* Initialized the features for this endpoint */
    conn->GetFeatures().isBusToBus = false;
    conn->GetFeatures().isBusToBus = false;
    conn->GetFeatures().handlePassing = false;

    /* Run the actual connection authentication code. */
    qcc::String authName;
    qcc::String redirection;
    status = conn->Establish("ANONYMOUS", authName, redirection);
    if (status != ER_OK) {
        conn->m_stream.Close();
        conn->m_state = FAILED;
        QCC_LogError(status, ("Failed to establish TCP endpoint"));
        return (void*)status;
    }

    /* Tell the server that the authentication succeeded and that it can bring the connection up. */
    conn->m_state = SUCCEEDED;
    conn->m_transport->Authenticated(conn);
    QCC_DbgTrace(("DaemonTCPEndpoint::AuthThread::Run(): Returning"));
    return (void*)status;
}


DaemonTCPTransport::DaemonTCPTransport(BusAttachment& bus)
    : Thread("DaemonTCPTransport"), m_bus(bus), m_ns(0), m_stopping(false), m_listener(0), m_foundCallback(m_listener)
{
    QCC_DbgTrace(("DaemonTCPTransport::DaemonTCPTransport()"));
    /*
     * We know we are daemon code, so we'd better be running with a daemon
     * router.  This is assumed elsewhere.
     */
    assert(m_bus.GetInternal().GetRouter().IsDaemon());
}

DaemonTCPTransport::~DaemonTCPTransport()
{
    QCC_DbgTrace(("DaemonTCPTransport::~DaemonTCPTransport()"));
    Stop();
    Join();
    delete m_ns;
    m_ns = 0;
}

void DaemonTCPTransport::Authenticated(DaemonTCPEndpoint* conn)
{
    QCC_DbgTrace(("DaemonTCPTransport::Authenticated()"));

    m_endpointListLock.Lock(MUTEX_CONTEXT);

    /*
     * If Authenticated() is being called, it is as a result of an
     * authentication thread deciding to do so.  This means it is running.  The
     * only places a connection may be removed from the m_authList is in the
     * case of a failed thread start, the thread exit function or here.  Since
     * the thead must be running to call us here, we must find the conn in the
     * m_authList or someone isn't playing by the rules.
     */
    list<DaemonTCPEndpoint*>::iterator i = find(m_authList.begin(), m_authList.end(), conn);
    assert(i != m_authList.end() && "DaemonTCPTransport::Authenticated(): Can't find connection");

    /*
     * We now transfer the responsibility for the connection data structure
     * to the m_endpointList.
     */
    m_authList.erase(i);
    m_endpointList.push_back(conn);

    /*
     * The responsibility for the connection data structure has been transferred
     * to the m_endpointList.  Before leaving we have to spin up the connection
     * threads which will actually assume the responsibility.  If the Start()
     * succeeds, those threads have it, but if Start() fails, we still do; and
     * there's not much we can do but give up.
     */
    conn->SetListener(this);
    QStatus status = conn->Start();
    if (status != ER_OK) {
        i = find(m_endpointList.begin(), m_endpointList.end(), conn);
        assert(i != m_authList.end() && "DaemonTCPTransport::Authenticated(): Can't find connection");
        m_authList.erase(i);
        delete conn;
        QCC_LogError(status, ("DaemonTCPTransport::Authenticated(): Failed to start TCP endpoint"));
    }
    m_endpointListLock.Unlock(MUTEX_CONTEXT);
}

QStatus DaemonTCPTransport::Start()
{
    /* TODO - need to read these values from the configuration file */
    const bool enableIPv4 = true;
    const bool enableIPv6 = true;

    QCC_DbgTrace(("DaemonTCPTransport::Start() ipv4=%s ipv6=%s", enableIPv4 ? "true" : "false", enableIPv6 ? "true" : "false"));

    /*
     * We rely on the status of the server accept thead as the primary
     * gatekeeper.
     *
     * A true response from IsRunning tells us that the server accept thread is
     * STARTED, RUNNING or STOPPING.
     *
     * When a thread is created it is in state INITIAL.  When an actual tread is
     * spun up as a result of Start(), it becomes STARTED.  Just before the
     * user's Run method is called, the thread becomes RUNNING.  If the Run
     * method exits, the thread becomes STOPPING.  When the thread is Join()ed
     * it becomes DEAD.
     *
     * IsRunning means that someone has called Thread::Start() and the process
     * has progressed enough that the thread has begun to execute.  If we get
     * multiple Start() calls calls on multiple threads, this test may fail to
     * detect multiple starts in a failsafe way and we may end up with multiple
     * server accept threads running.  We assume that since Start() requests
     * come in from our containing transport list it will not allow concurrent
     * start requests.
     */
    if (IsRunning()) {
        QCC_LogError(ER_BUS_BUS_ALREADY_STARTED, ("DaemonTCPTransport::Start(): Already started"));
        return ER_BUS_BUS_ALREADY_STARTED;
    }

    /*
     * In order to pass the IsRunning() gate above, there must be no server
     * accept thread running.  Running includes a thread that has been asked to
     * stop but has not been Join()ed yet.  So we know that there is no thread
     * and that either a Start() has never happened, or a Start() followed by a
     * Stop() and a Join() has happened.  Since Join() does a Thread::Join and
     * then deletes the name service, it is possible that a Join() done on one
     * thread is done enough to pass the gate above, but has not yet finished
     * deleting the name service instance when a Start() comes in on another
     * thread.  Because of this (rare and unusual) possibility we also check the
     * name service instance and return an error if we find it non-zero.  If the
     * name service is NULL, the Stop() and Join() is totally complete and we
     * can safely proceed.
     */
    if (m_ns != NULL) {
        QCC_LogError(ER_BUS_BUS_ALREADY_STARTED, ("DaemonTCPTransport::Start(): Name service already started"));
        return ER_BUS_BUS_ALREADY_STARTED;
    }

    m_ns = new NameService;
    assert(m_ns);

    m_stopping = false;

    /*
     * We have a configuration item that controls whether or not to use IPv4
     * broadcasts, so we need to check it now and give it to the name service as
     * we bring it up.
     */
    bool disable = false;
    if (ConfigDB::GetConfigDB()->GetProperty(NameService::MODULE_NAME, NameService::BROADCAST_PROPERTY) == "true") {
        disable = true;
    }

    /*
     * Get the guid from the bus attachment which will act as the globally unique
     * ID of the daemon.
     */
    qcc::String guidStr = m_bus.GetInternal().GetGlobalGUID().ToString();

    QStatus status = m_ns->Init(guidStr, enableIPv4, enableIPv6, disable);
    if (status != ER_OK) {
        QCC_LogError(status, ("DaemonTCPTransport::Start(): Error starting name service"));
        return status;
    }

    /*
     * Tell the name service to call us back on our FoundCallback method when
     * we hear about a new well-known bus name.
     */
    m_ns->SetCallback(
        new CallbackImpl<FoundCallback, void, const qcc::String&, const qcc::String&, std::vector<qcc::String>&, uint8_t>
            (&m_foundCallback, &FoundCallback::Found));

    /*
     * Start the server accept loop through the thread base class.  This will
     * close or open the IsRunning() gate we use to control access to our
     * public API.
     */
    return Thread::Start();
}

QStatus DaemonTCPTransport::Stop(void)
{
    QCC_DbgTrace(("DaemonTCPTransport::Stop()"));

    /*
     * It is legal to call Stop() more than once, so it must be possible to
     * call Stop() on a stopped transport.
     */
    m_stopping = true;

    /*
     * Tell the name service to stop calling us back if it's there (we may get
     * called more than once in the chain of destruction) so the pointer is not
     * required to be non-NULL.
     */
    if (m_ns) {
        m_ns->SetCallback(NULL);
    }

    /*
     * Tell the server accept loop thread to shut down through the thead
     * base class.
     */
    QStatus status = Thread::Stop();
    if (status != ER_OK) {
        QCC_LogError(status, ("DaemonTCPTransport::Stop(): Failed to Stop() server thread"));
        return status;
    }

    m_endpointListLock.Lock(MUTEX_CONTEXT);

    /*
     * Ask any authenticating endpoints to shut down and exit their threads.  By its
     * presence on the m_authList, we know that the endpoint is authenticating and
     * the authentication thread has responsibility for dealing with the endpoint
     * data structure.  We call Abort() to stop that thread from running.  The
     * endpoint Rx and Tx threads will not be running yet.
     */
    for (list<DaemonTCPEndpoint*>::iterator i = m_authList.begin(); i != m_authList.end(); ++i) {
        (*i)->Abort();
    }

    /*
     * Ask any running endpoints to shut down and exit their threads.  By its
     * presence on the m_endpointList, we know that authentication is compete and
     * the Rx and Tx threads have responsibility for dealing with the endpoint
     * data structure.  We call Stop() to stop those threads from running.  Since
     * the connnection is on the m_endpointList, we know that the authentication
     * thread has handed off responsibility.
     */
    for (list<DaemonTCPEndpoint*>::iterator i = m_endpointList.begin(); i != m_endpointList.end(); ++i) {
        (*i)->Stop();
    }

    m_endpointListLock.Unlock(MUTEX_CONTEXT);

    /*
     * The use model for DaemonTCPTransport is that it works like a thread.
     * There is a call to Start() that spins up the server accept loop in order
     * to get it running.  When someone wants to tear down the transport, they
     * call Stop() which requests the transport to stop.  This is followed by
     * Join() which waits for all of the threads to actually stop.
     *
     * The name service should play by those rules as well.  We allocate and
     * initialize it in Start(), which will spin up the main thread there.
     * We need to Stop() the name service here and Join its thread in
     * DaemonTCPTransport::Join().  If someone just deletes the transport
     * there is an implied Stop() and Join() so it behaves correctly.
     */
    if (m_ns) {
        m_ns->Stop();
    }

    return ER_OK;
}

QStatus DaemonTCPTransport::Join(void)
{
    QCC_DbgTrace(("DaemonTCPTransport::Join()"));

    /*
     * It is legal to call Join() more than once, so it must be possible to
     * call Join() on a joined transport.
     *
     * First, wait for the server accept loop thread to exit.
     */
    QStatus status = Thread::Join();
    if (status != ER_OK) {
        QCC_LogError(status, ("DaemonTCPTransport::Join(): Failed to Join() server thread"));
        return status;
    }

    /*
     * A requred call to Stop() that needs to happen before this Join will ask
     * all of the endpoints to stop; and will also cause any authenticating
     * endpoints to stop.  We still need to wait here until all of the threads
     * running in those endpoints actually stop running.
     *
     * Since Stop() is a request to stop, and this is what has ultimately been
     * done to both authentication threads and Rx and Tx threads, it is possible
     * that a thread is actually running after the call to Stop().  If that
     * thead happens to be an authenticating endpoint, it is possible that an
     * authentication actually completes after Stop() is called.  This will move
     * a connection from the m_authList to the m_endpointList, so we need to
     * make sure we wait for all of the connections on the m_authList to go away
     * before we look for the connections on the m_endpointlist.
     */
    m_endpointListLock.Lock(MUTEX_CONTEXT);
    while (m_authList.size() > 0) {
        m_endpointListLock.Unlock(MUTEX_CONTEXT);
        /*
         * Sleep(0) yields to threads of equal or higher priority, so we use
         * Sleep(1) to make sure we actually yield.  Since the OS has its own
         * idea of granulatity this will actually be more -- on Linux, for
         * example, this will translate into 1 Jiffy, which is probably 1/250
         * sec or 4 ms.
         */
        qcc::Sleep(1);
        m_endpointListLock.Lock(MUTEX_CONTEXT);
    }

    /* We need to wait here until all of the threads running in the previously
     * authenticated endpoints actually stop running.  When a remote endpoint
     * thead exits the endpoint will call back into our EndpointExit() and have
     * itself removed from the m_endpointList and clean up by themselves.
     */
    while (m_endpointList.size() > 0) {
        m_endpointListLock.Unlock(MUTEX_CONTEXT);
        qcc::Sleep(1);
        m_endpointListLock.Lock(MUTEX_CONTEXT);
    }

    /*
     * Under no condition will we leave a thread running when we exit this
     * function.
     */
    assert(m_authList.size() == 0);
    assert(m_endpointList.size() == 0);

    m_endpointListLock.Unlock(MUTEX_CONTEXT);

    /*
     * The use model for DaemonTCPTransport is that it works like a thread.
     * There is a call to Start() that spins up the server accept loop in order
     * to get it running.  When someone wants to tear down the transport, they
     * call Stop() which requests the transport to stop.  This is followed by
     * Join() which waits for all of the threads to actually stop.
     *
     * The name service needs to play by the use model for the transport (see
     * Start()).  We allocate and initialize it in Start() so we need to Join
     * and delete the name service here.  Since there is an implied Join() in
     * the destructor we just delete the name service to play by the rules.
     */
    delete m_ns;
    m_ns = NULL;

    m_stopping = false;

    return ER_OK;
}

/*
 * The default interface for the name service to use.  The wildcard character
 * means to listen and transmit over all interfaces that are up and multicast
 * capable, with any IP address they happen to have.  This default also applies
 * to the search for listen address interfaces.
 */
static const char* INTERFACES_DEFAULT = "*";

QStatus DaemonTCPTransport::GetListenAddresses(const SessionOpts& opts, std::vector<qcc::String>& busAddrs) const
{
    QCC_DbgTrace(("DaemonTCPTransport::GetListenAddresses()"));

    /*
     * We are given a session options structure that defines the kind of
     * transports that are being sought.  TCP provides reliable traffic as
     * understood by the session options, so we only return someting if
     * the traffic type is TRAFFIC_MESSAGES or TRAFFIC_RAW_RELIABLE.  It's
     * not an error if we don't match, we just don't have anything to offer.
     */
    if (opts.traffic != SessionOpts::TRAFFIC_MESSAGES && opts.traffic != SessionOpts::TRAFFIC_RAW_RELIABLE) {
        QCC_DbgPrintf(("DaemonTCPTransport::GetListenAddresses(): traffic mismatch"));
        return ER_OK;
    }

    /*
     * The other session option that we need to filter on is the transport
     * bitfield.  We have no easy way of figuring out if we are a wireless
     * local-area, wireless wide-area, wired local-area or local transport,
     * but we do exist, so we respond if the caller is asking for any of
     * those: cogito ergo some.
     */
    if (!(opts.transports & (TRANSPORT_WLAN | TRANSPORT_WWAN | TRANSPORT_LAN))) {
        QCC_DbgPrintf(("DaemonTCPTransport::GetListenAddresses(): transport mismatch"));
        return ER_OK;
    }

    /*
     * The name service is allocated in Start(), Started by the call to Init()
     * in Start(), Stopped in our Stop() method and deleted in our Join().  In
     * this case, the transport will probably be started, and we will probably
     * find m_ns set, but there is no requirement to ensure this.  If m_ns is
     * NULL, we need to complain so the user learns to Start() the transport
     * before calling IfConfig.  A call to IsRunning() here is superfluous since
     * we really don't care about anything but the name service in this method.
     */
    if (m_ns == NULL) {
        QCC_LogError(ER_BUS_TRANSPORT_NOT_STARTED, ("DaemonTCPTransport::GetListenAddresses(): NameService not initialized"));
        return ER_BUS_TRANSPORT_NOT_STARTED;
    }

    /*
     * Our goal is here is to match a list of interfaces provided in the
     * configuration database (or a wildcard) to a list of interfaces that are
     * IFF_UP in the system.  The first order of business is to get the list of
     * interfaces in the system.  We do that using a convenient OS-inependent
     * call into the name service.
     *
     * We can't cache this list since it may change as the phone wanders in
     * and out of range of this and that and the underlying IP addresses change
     * as DHCP doles out whatever it feels like at any moment.
     */
    QCC_DbgPrintf(("DaemonTCPTransport::GetListenAddresses(): IfConfig()"));

    std::vector<qcc::IfConfigEntry> entries;
    QStatus status = qcc::IfConfig(entries);
    if (status != ER_OK) {
        QCC_LogError(status, ("DaemonTCPTransport::GetListenAddresses(): ns.IfConfig() failed"));
        return status;
    }

    /*
     * The next thing to do is to get the list of interfaces from the config
     * file.  These are required to be formatted in a comma separated list,
     * with '*' being a wildcard indicating that we want to match any interface.
     * If there is no configuration item, we default to something rational.
     */
    QCC_DbgPrintf(("DaemonTCPTransport::GetListenAddresses(): GetProperty()"));
    qcc::String interfaces = ConfigDB::GetConfigDB()->GetProperty(NameService::MODULE_NAME, NameService::INTERFACES_PROPERTY);
    if (interfaces.size() == 0) {
        interfaces = INTERFACES_DEFAULT;
    }

    /*
     * Check for wildcard anywhere in the configuration string.  This trumps
     * anything else that may be there and ensures we get only one copy of
     * the addresses if someone tries to trick us with "*,*".
     */
    bool haveWildcard = false;
    const char*wildcard = "*";
    size_t i = interfaces.find(wildcard);
    if (i != qcc::String::npos) {
        QCC_DbgPrintf(("DaemonTCPTransport::GetListenAddresses(): wildcard search"));
        haveWildcard = true;
        interfaces = wildcard;
    }

    /*
     * Walk the comma separated list from the configuration file and and try
     * to mach it up with interfaces actually found in the system.
     */
    while (interfaces.size()) {
        /*
         * We got a comma-separated list, so we need to work our way through
         * the list.  Each entry in the list  may be  an interface name, or a
         * wildcard.
         */
        qcc::String currentInterface;
        size_t i = interfaces.find(",");
        if (i != qcc::String::npos) {
            currentInterface = interfaces.substr(0, i);
            interfaces = interfaces.substr(i + 1, interfaces.size() - i - 1);
        } else {
            currentInterface = interfaces;
            interfaces.clear();
        }

        QCC_DbgPrintf(("DaemonTCPTransport::GetListenAddresses(): looking for interface %s", currentInterface.c_str()));

        /*
         * Walk the list of interfaces that we got from the system and see if
         * we find a match.
         */
        for (uint32_t i = 0; i < entries.size(); ++i) {
            QCC_DbgPrintf(("DaemonTCPTransport::GetListenAddresses(): matching %s", entries[i].m_name.c_str()));
            /*
             * To match a configuration entry, the name of the interface must:
             *
             *   - match the name in the currentInterface (or be wildcarded);
             *   - be UP which means it has an IP address assigned;
             *   - not be the LOOPBACK device and therefore be remotely available.
             */
            uint32_t mask = qcc::IfConfigEntry::UP |
                            qcc::IfConfigEntry::LOOPBACK;

            uint32_t state = qcc::IfConfigEntry::UP;

            if ((entries[i].m_flags & mask) == state) {
                QCC_DbgPrintf(("DaemonTCPTransport::GetListenAddresses(): %s has correct state", entries[i].m_name.c_str()));
                if (haveWildcard || entries[i].m_name == currentInterface) {
                    QCC_DbgPrintf(("DaemonTCPTransport::GetListenAddresses(): %s has correct name", entries[i].m_name.c_str()));
                    /*
                     * This entry matches our search criteria, so we need to
                     * turn the IP address that we found into a busAddr.  We
                     * must be a TCP transport, and we have an IP address
                     * already in a string, so we can easily put together the
                     * desired busAddr.
                     */
                    QCC_DbgTrace(("DaemonTCPTransport::GetListenAddresses(): %s match found", entries[i].m_name.c_str()));
                    /*
                     * We know we have an interface that speaks IP and
                     * which has an IP address we can pass back. We know
                     * it is capable of receiving incoming connections, but
                     * the $64,000 questions are, does it have a listener
                     * and what port is that listener listening on.
                     *
                     * There is one name service associated with the daemon
                     * TCP transport, and it is advertising at most one port.
                     * It may be advertising that port over multiple
                     * interfaces, but there is currently just one port being
                     * advertised.  If multiple listeners are created, the
                     * name service only advertises the lastly set port.  In
                     * the future we may need to add the ability to advertise
                     * different ports on different interfaces, but the answer
                     * is simple now.  Ask the name service for the one port
                     * it is advertising and that must be the answer.
                     */
                    qcc::String ipv4address;
                    qcc::String ipv6address;
                    uint16_t port;
                    m_ns->GetEndpoints(ipv4address, ipv6address, port);
                    /*
                     * If the port is zero, then it hasn't been set and this
                     * implies that DaemonTCPTransport::StartListen hasn't
                     * been called and there is no listener for this transport.
                     * We should only return an address if we have a listener.
                     */
                    if (port) {
                        /*
                         * Now put this information together into a bus address
                         * that the rest of the AllJoyn world can understand.
                         */
                        if (!ipv4address.empty()) {
                            qcc::String busAddr = "tcp:addr=" + entries[i].m_addr + ",port=" + U32ToString(port) + ",family=ipv4";
                            busAddrs.push_back(busAddr);
                        }
                        if (!ipv6address.empty()) {
                            qcc::String busAddr = "tcp:addr=" + entries[i].m_addr + ",port=" + U32ToString(port) + ",family=ipv6";
                            busAddrs.push_back(busAddr);
                        }
                    }
                }
            }
        }
    }

    /*
     * If we can get the list and walk it, we have succeeded.  It is not an
     * error to have no available interfaces.  In fact, it is quite expected
     * in a phone if it is not associated with an access point over wi-fi.
     */
    QCC_DbgPrintf(("DaemonTCPTransport::GetListenAddresses(): done"));
    return ER_OK;
}

void DaemonTCPTransport::EndpointExit(RemoteEndpoint* ep)
{
    /*
     * This is a callback driven from the remote endpoint thread exit function.
     * Our DaemonTCPEndpoint inherits from class RemoteEndpoint and so when
     * either of the threads (transmit or receive) of one of our endpoints exits
     * for some reason, we get called back here.
     */
    QCC_DbgTrace(("DaemonTCPTransport::EndpointExit()"));

    DaemonTCPEndpoint* tep = static_cast<DaemonTCPEndpoint*>(ep);
    assert(tep);

    /* Remove the dead endpoint from the live endpoint list */
    m_endpointListLock.Lock(MUTEX_CONTEXT);
    list<DaemonTCPEndpoint*>::iterator i = find(m_endpointList.begin(), m_endpointList.end(), tep);
    if (i != m_endpointList.end()) {
        m_endpointList.erase(i);
    }
    m_endpointListLock.Unlock(MUTEX_CONTEXT);

    /*
     * The endpoint can exit if it was asked to by us in response to a Disconnect()
     * from higher level code, or if it got an error from the underlying transport.
     * We need to notify upper level code if the disconnect is due to an event from
     * the transport.
     */
    if (m_listener && tep->IsSuddenDisconnect()) {
        m_listener->BusConnectionLost(tep->GetConnectSpec());
    }

    delete tep;
}

void* DaemonTCPTransport::Run(void* arg)
{
    QCC_DbgTrace(("DaemonTCPTransport::Run()"));
    /*
     * This is the Thread Run function for our server accept loop.  We require
     * that the name service be started before the Thread that will call us
     * here.
     */
    assert(m_ns);

    /*
     * We need to find the defaults for our connection limits.  These limits
     * can be specified in the configuration database with corresponding limits
     * used for DBus.  If any of those are present, we use them, otherwise we
     * provide some hopefully reasonable defaults.
     */
    ConfigDB* config = ConfigDB::GetConfigDB();

    /*
     * tTimeout is the maximum amount of time we allow incoming connections to
     * mess about while they should be authenticating.  If they take longer
     * than this time, we feel free to disconnect them as deniers of service.
     */
    uint32_t authTimeoutConfig = config->GetLimit("auth_timeout");
    Timespec tTimeout = authTimeoutConfig ? authTimeoutConfig : ALLJOYN_AUTH_TIMEOUT_DEFAULT;

    /*
     * maxAuth is the maximum number of incoming connections that can be in
     * the process of authenticating.  If starting to authenticate a new
     * connection would mean exceeding this number, we drop the new connection.
     */
    uint32_t maxAuthConfig = config->GetLimit("max_incomplete_connections_tcp");
    uint32_t maxAuth = maxAuthConfig ? maxAuthConfig : ALLJOYN_MAX_INCOMPLETE_CONNECTIONS_TCP_DEFAULT;

    /*
     * maxConn is the maximum number of active connections possible over the
     * TCP transport.  If starting to process a new connection would mean
     * exceeding this number, we drop the new connection.
     */
    uint32_t maxConnConfig = config->GetLimit("max_completed_connections_tcp");
    uint32_t maxConn = maxConnConfig ? maxConnConfig : ALLJOYN_MAX_COMPLETED_CONNECTIONS_TCP_DEFAULT;

    QStatus status = ER_OK;

    while (!IsStopping()) {
        /*
         * We require that the name service be created and started before the
         * Thread that called us here; and we require that the name service stay
         * around until after we leave.
         */
        assert(m_ns);

        /*
         * Each time through the loop we create a set of events to wait on.
         * We need to wait on the stop event and all of the SocketFds of the
         * addresses and ports we are listening on.  If the list changes, the
         * code that does the change Alert()s this thread and we wake up and
         * re-evaluate the list of SocketFds.
         */
        m_listenFdsLock.Lock(MUTEX_CONTEXT);
        vector<Event*> checkEvents, signaledEvents;
        checkEvents.push_back(&stopEvent);
        for (list<pair<qcc::String, SocketFd> >::const_iterator i = m_listenFds.begin(); i != m_listenFds.end(); ++i) {
            checkEvents.push_back(new Event(i->second, Event::IO_READ, false));
        }
        m_listenFdsLock.Unlock(MUTEX_CONTEXT);

        /*
         * We have our list of events, so now wait for something to happen
         * on that list (or get alerted).
         */
        signaledEvents.clear();

        status = Event::Wait(checkEvents, signaledEvents);
        if (ER_OK != status) {
            QCC_LogError(status, ("Event::Wait failed"));
            break;
        }

        /*
         * We're back from our Wait() so one of three things has happened.  Our
         * thread has been asked to Stop(), our thread has been Alert()ed, or
         * one of the socketFds we are listening on for connecte events has
         * becomed signalled.
         *
         * If we have been asked to Stop(), or our thread has been Alert()ed,
         * the stopEvent will be on the list of signalled events.  The
         * difference can be found by a call to IsStopping() which is found
         * above.  An alert means that a request to start or stop listening
         * on a given address and port has been queued up for us.
         */
        for (vector<Event*>::iterator i = signaledEvents.begin(); i != signaledEvents.end(); ++i) {
            /*
             * Reset an existing Alert() or Stop().  If it's an alert, we
             * will deal with looking for the incoming listen requests at
             * the bottom of the server loop.  If it's a stop we will
             * exit the next time through the top of the server loop.
             */
            if (*i == &stopEvent) {
                stopEvent.ResetEvent();
                continue;
            }

            /*
             * Since the current event is not the stop event, it must reflect at
             * least one of the SocketFds we are waiting on for incoming
             * connections.  Go ahead and Accept() the new connection on the
             * current SocketFd.
             */
            IPAddress remoteAddr;
            uint16_t remotePort;
            SocketFd newSock;

            status = Accept((*i)->GetFD(), remoteAddr, remotePort, newSock);
            if (status == ER_OK) {
                QCC_DbgHLPrintf(("DaemonTCPTransport::Run(): Accepting connection"));

                /*
                 * We have a request for a new connection.  We need to
                 * Authenticate before naively allowing, and we can't do
                 * blocking calls here, so we need to spin up a thread to
                 * handle them.  We can't allow a malicious user to cause
                 * us to spin up threads till we kill the phone, so we
                 * have a list of pending authorizations.  We also need to
                 * time out possibly malicious connection requests that will
                 * never complete, so we can timeout the least recently used
                 * request.  Finally, we need to lazily clean up any
                 * connections that have failed authentication.
                 *
                 * Does not handle rollover, but a Timespec holds uint32_t
                 * worth of seconds that derives from the startup time of the
                 * system in the Posix case or the number of seconds since
                 * jan 1, 1970 in the Windows case.  This is 136 years worth
                 * of seconds which means we're okay `till the year 2106.
                 */
                Timespec tNow;
                GetTimeNow(&tNow);

                QCC_DbgHLPrintf(("DaemonTCPTransport connect request"));

                m_endpointListLock.Lock(MUTEX_CONTEXT);

                /*
                 * See if there any pending connections on the list that can be
                 * removed because they timed out or failed.  If the connection is
                 * on the pending authentication list, we assume that there is an
                 * authentication thread running which we can abort. If we bug
                 * Abort(), we are *asking* an in-process authentication to
                 * stop.  When it does, it will delete itself from the
                 * m_authList and go away.
                 *
                 * Here's the trick: It is holding real resources, and may take
                 * time to release them and exit (for example, close a stream).
                 * We can't very well just stop the server loop to wait for a
                 * problematic connection to un-hose itself, but what we can do
                 * is yield the CPU in the hope that the problem connection
                 * closes down immediately.  Sleep(0) yields to threads of equal
                 * or higher priority, so we use Sleep(1) to make sure we
                 * actually yield to everyone.  Since the OS has its own idea of
                 * granulatity this will be more -- on Linux, this will
                 * translate into 1 Jiffy, which is probably 1/250 sec or 4 ms.
                 */
                QCC_DbgPrintf(("DaemonTCPTransport::Run(): maxAuth == %d", maxAuth));
                QCC_DbgPrintf(("DaemonTCPTransport::Run(): maxConn == %d", maxConn));
                QCC_DbgPrintf(("DaemonTCPTransport::Run(): mAuthList.size() == %d", m_authList.size()));
                QCC_DbgPrintf(("DaemonTCPTransport::Run(): mEndpointList.size() == %d", m_endpointList.size()));
                assert(m_authList.size() + m_endpointList.size() <= maxConn);

                /*
                 * Run through the list of authenticating endpoints and scavenge
                 * any that are failed or are taking too long (denial of service
                 * attack assumed).
                 */
                list<DaemonTCPEndpoint*>::iterator j = m_authList.begin();
                while (j != m_authList.end()) {
                    DaemonTCPEndpoint* ep = *j;
                    if (ep->IsFailed() && !ep->IsAuthThreadRunning()) {
                        /*
                         * The straightforward case is if the endpoint failed
                         * authentication.  Then the auth thread will exit on
                         * its own.  We can delete the endpoint as soon as the
                         * thead is gone.
                         */
                        QCC_DbgHLPrintf(("DaemonTCPTransport::Run(): Scavenging failed authenticator"));
                        j = m_authList.erase(j);
                        delete ep;
                        ep = NULL;
                    } else if (ep->GetStartTime() + tTimeout < tNow) {
                        /*
                         * A less straightforward case is if the endpoint is
                         * taking too long to authenticate.  What we do is abort
                         * the authentication process.  If the authentication
                         * thread is in the middle of something, this Abort()
                         * will cause a blocking operation to fail and will
                         * cause the authentiction thread to set its status to
                         * FAILED.  Then the endpoint will be scavenged the next
                         * time through the loop immediately above.  If we
                         * happen to be too late to affect the thread via a
                         * blocking operation it will actually succeed and exit
                         * through the SUCCEEDED mechanism calling Authenticated()
                         * which will result in the endpoint being taken off of
                         * the authList, which is what we want.
                         */
                        QCC_DbgHLPrintf(("DaemonTCPTransport::Run(): Scavenging slow authenticator"));
                        ep->Abort();
                    } else {
                        ++j;
                    }
                }

                /*
                 * We've scavenged any slots we can, so the question now is, do
                 * we have a slot available for a new connection?  If so, use
                 * it.
                 */
                if ((m_authList.size() < maxAuth) && (m_authList.size() + m_endpointList.size() < maxConn)) {
                    DaemonTCPEndpoint* conn = new DaemonTCPEndpoint(this, m_bus, true, "", newSock, remoteAddr, remotePort);
                    Timespec tNow;
                    GetTimeNow(&tNow);
                    conn->SetStartTime(tNow);
                    /*
                     * By putting the connection on the m_authList, we are
                     * transferring responsibility for the connection to the
                     * Authentication thread.  Therefore, we must check that the
                     * thread actually started running to ensure the handoff
                     * worked.  If it didn't we need to deal with the connection
                     * here.
                     */
                    m_authList.push_front(conn);
                    status = conn->Authenticate();
                    if (status != ER_OK) {
                        m_authList.pop_front();
                        delete conn;
                        conn = NULL;
                    }
                    conn = NULL;
                } else {
                    qcc::Shutdown(newSock);
                    qcc::Close(newSock);
                    status = ER_AUTH_FAIL;
                    QCC_LogError(status, ("DaemonTCPTransport::Run(): No slot for new connection"));
                }

                m_endpointListLock.Unlock(MUTEX_CONTEXT);
            } else if (ER_WOULDBLOCK == status) {
                status = ER_OK;
            }

            if (ER_OK != status) {
                QCC_LogError(status, ("DaemonTCPTransport::Run(): Error accepting new connection. Ignoring..."));
            }
        }

        /*
         * We're going to loop back and create a new list of checkEvents that
         * reflect the current state, so we need to delete the checkEvents we
         * created on this iteration.
         */
        for (vector<Event*>::iterator i = checkEvents.begin(); i != checkEvents.end(); ++i) {
            if (*i != &stopEvent) {
                delete *i;
            }
        }

        /*
         * If we're not stopping, we always check for queued requests to
         * start and stop listening on address and port combinations (listen
         * specs).  We do that here since we have just deleted all of the
         * events that may have references to our socket FD resources which
         * may be released as a result of a DoStopListen() call.
         *
         * When we loop back to the top of the server accept loop, we will
         * re-evaluate the list of listenFds and create new events based on the
         * current state of the list (after we remove or add anything here).
         */
        while (m_listenRequests.empty() == false) {
            ListenRequest listenRequest = m_listenRequests.front();
            m_listenRequests.pop();
            switch (listenRequest.m_request) {
            case START_LISTEN:
                DoStartListen(listenRequest.m_listenSpec);
                break;

            case STOP_LISTEN:
                DoStopListen(listenRequest.m_listenSpec);
                break;

            default:
                assert(false && "DaemonTCPTransport::Run(): unexpected listen request code");
            }
        }
    }

    /*
     * If we're stopping, it is our responsibility to clean up the list of FDs
     * we are listening to.  Since we've gotten a Stop() and are exiting the
     * server loop, and FDs are added in the server loop, this is the place to
     * get rid of them.  We don't have to take the list lock since a Stop()
     * request to the DaemonTCPTransport is required to lock out any new
     * requests that may possibly touch the listen FDs list.
     */
    m_listenFdsLock.Lock(MUTEX_CONTEXT);
    for (list<pair<qcc::String, SocketFd> >::iterator i = m_listenFds.begin(); i != m_listenFds.end(); ++i) {
        qcc::Shutdown(i->second);
        qcc::Close(i->second);
    }
    m_listenFds.clear();
    m_listenFdsLock.Unlock(MUTEX_CONTEXT);

    QCC_DbgPrintf(("DaemonTCPTransport::Run is exiting status=%s", QCC_StatusText(status)));
    return (void*) status;
}

/*
 * The default address for use in listen specs.  INADDR_ANY means to listen
 * for TCP connections on any interfaces that are currently up or any that may
 * come up in the future.
 */
static const char* ADDR4_DEFAULT = "0.0.0.0";
static const char* ADDR6_DEFAULT = "0::0";

/*
 * The default port for use in listen specs.  This port is used by the TCP
 * listener to listen for incoming connection requests.
 */
#ifdef QCC_OS_ANDROID
static const uint16_t PORT_DEFAULT = 0;
#else
static const uint16_t PORT_DEFAULT = 9955;
#endif

QStatus DaemonTCPTransport::NormalizeListenSpec(const char* inSpec, qcc::String& outSpec, map<qcc::String, qcc::String>& argMap) const
{
    qcc::String family;

    /*
     * We don't make any calls that require us to be in any particular state
     * with respect to threading so we don't bother to call IsRunning() here.
     *
     * Take the string in inSpec, which must start with "tcp:" and parse it,
     * looking for comma-separated "key=value" pairs and initialize the
     * argMap with those pairs.
     */
    QStatus status = ParseArguments("tcp", inSpec, argMap);
    if (status != ER_OK) {
        return status;
    }
    /*
     * If the family was specified we will check that address matches otherwise
     * we will figure out the family from the address format.
     */
    map<qcc::String, qcc::String>::iterator iter = argMap.find("family");
    if (iter != argMap.end()) {
        family = iter->second;
    }

    iter = argMap.find("addr");
    if (iter == argMap.end()) {
        if (family.empty()) {
            family = "ipv4";
        }
        qcc::String addrString = (family == "ipv6") ? ADDR6_DEFAULT : ADDR4_DEFAULT;
        argMap["addr"] = addrString;
        outSpec = "tcp:addr=" + addrString;
    } else {
        /*
         * We have a value associated with the "addr" key.  Run it through
         * a conversion function to make sure it's a valid value.
         */
        IPAddress addr;
        status = addr.SetAddress(iter->second, false);
        if (status == ER_OK) {
            if (family.empty()) {
                family = addr.IsIPv6() ? "ipv6" : "ipv4";
            } else if (addr.IsIPv6() != (family == "ipv6")) {
                return ER_BUS_BAD_TRANSPORT_ARGS;
            }
            // Normalize address representation
            iter->second = addr.ToString();
            outSpec = "tcp:addr=" + iter->second;
        } else {
            return ER_BUS_BAD_TRANSPORT_ARGS;
        }
    }
    argMap["family"] = family;
    outSpec += ",family=" + family;

    iter = argMap.find("port");
    if (iter == argMap.end()) {
        qcc::String portString = U32ToString(PORT_DEFAULT);
        argMap["port"] = portString;
        outSpec += ",port=" + portString;
    } else {
        /*
         * We have a value associated with the "port" key.  Run it through
         * a conversion function to make sure it's a valid value.
         */
        uint32_t port = StringToU32(iter->second);
        if (port > 0 && port <= 0xffff) {
            iter->second = U32ToString(port);
            outSpec += ",port=" + iter->second;
        } else {
            return ER_BUS_BAD_TRANSPORT_ARGS;
        }
    }

    return ER_OK;
}

QStatus DaemonTCPTransport::NormalizeTransportSpec(const char* inSpec, qcc::String& outSpec, map<qcc::String, qcc::String>& argMap) const
{
    /*
     * We don't make any calls that require us to be in any particular state
     * with respect to threading so we don't bother to call IsRunning() here.
     *
     * Unlike a listenSpec a transportSpec (actually a connectSpec) must have
     * a specific address (INADDR_ANY isn't a valid IP address to connect to).
     */
    QStatus status = NormalizeListenSpec(inSpec, outSpec, argMap);
    if (status != ER_OK) {
        return status;
    }

    /*
     * Since the only difference between a transportSpec and a listenSpec is
     * the presence of the address, we just check for the default address
     * and fail if we find it.
     */
    map<qcc::String, qcc::String>::iterator i = argMap.find("addr");
    assert(i != argMap.end());
    if ((i->second == ADDR4_DEFAULT) || (i->second == ADDR6_DEFAULT)) {
        return ER_BUS_BAD_TRANSPORT_ARGS;
    }

    return ER_OK;
}

QStatus DaemonTCPTransport::Connect(const char* connectSpec, const SessionOpts& opts, RemoteEndpoint** newep)
{
    QCC_DbgHLPrintf(("DaemonTCPTransport::Connect(): %s", connectSpec));

    QStatus status;
    bool isConnected = false;

    /*
     * We only want to allow this call to proceed if we have a running server
     * accept thread that isn't in the process of shutting down.  We use the
     * thread response from IsRunning to give us an idea of what our server
     * accept (Run) thread is doing.  See the comment in Start() for details
     * about what IsRunning actually means, which might be subtly different from
     * your intuitition.
     *
     * If we see IsRunning(), the thread might actually have gotten a Stop(),
     * but has not yet exited its Run routine and become STOPPING.  To plug this
     * hole, we need to check IsRunning() and also m_stopping, which is set in
     * our Stop() method.
     */
    if (IsRunning() == false || m_stopping == true) {
        QCC_LogError(ER_BUS_TRANSPORT_NOT_STARTED, ("DaemonTCPTransport::Connect(): Not running or stopping; exiting"));
        return ER_BUS_TRANSPORT_NOT_STARTED;
    }

    /*
     * If we pass the IsRunning() gate above, we must have a server accept
     * thread spinning up or shutting down but not yet joined.  Since the name
     * service is created before the server accept thread is spun up, and
     * deleted after it is joined, we must have a valid name service or someone
     * isn't playing by the rules; so an assert is appropriate here.
     */
    assert(m_ns);

    /*
     * Parse and normalize the connectArgs.  When connecting to the outside
     * world, there are no reasonable defaults and so the addr and port keys
     * MUST be present.
     */
    qcc::String normSpec;
    map<qcc::String, qcc::String> argMap;
    status = NormalizeTransportSpec(connectSpec, normSpec, argMap);
    if (ER_OK != status) {
        QCC_LogError(status, ("TCPTransport::Connect(): Invalid TCP connect spec \"%s\"", connectSpec));
        return status;
    }
    /*
     * These fields (addr, port, family) are all guaranteed to be present
     */
    IPAddress ipAddr(argMap.find("addr")->second);
    uint16_t port = StringToU32(argMap["port"]);
    qcc::AddressFamily family = argMap["family"] == "ipv6" ?  QCC_AF_INET6 : QCC_AF_INET;

    /*
     * The semantics of the Connect method tell us that we want to connect to a
     * remote daemon.  TCP will happily allow us to connect to ourselves, but
     * this is not always possible in the various transports AllJoyn may use.
     * To avoid unnecessary differences, we do not allow a requested connection
     * to "ourself" to succeed.
     *
     * The code here is not a failsafe way to prevent this since thre are going
     * to be multiple processes involved that have no knowledge of what the
     * other is doing (for example, the wireless supplicant and this daemon).
     * This means we can't synchronize and there will be race conditions that
     * can cause the tests for selfness to fail.  The final check is made in the
     * bus hello protocol, which will abort the connection if it detects it is
     * conected to itself.  We just attempt to short circuit the process where
     * we can and not allow connections to proceed that will be bound to fail.
     *
     * One defintion of a connection to ourself is if we find that a listener
     * has has been started via a call to our own StartListener() with the same
     * connectSpec as we have now.  This is the simple case, but it also turns
     * out to be the uncommon case.
     *
     * It is perfectly legal to start a listener using the INADDR_ANY address,
     * which tells the system to listen for connections on any network interface
     * that happens to be up or that may come up in the future.  This is the
     * default listen address and is the most common case.  If this option has
     * been used, we expect to find a listener with a normalized adresss that
     * looks like "addr=0.0.0.0,port=y".  If we detect this kind of connectSpec
     * we have to look at the currently up interfaces and see if any of them
     * match the address provided in the connectSpec.  If so, we are attempting
     * to connect to ourself and we must fail that request.
     */
    char anyspec[40];
    if (family == QCC_AF_INET) {
        snprintf(anyspec, sizeof(anyspec), "tcp:addr=0.0.0.0,port=%u,family=ipv4", port);
    } else {
        snprintf(anyspec, sizeof(anyspec), "tcp:addr=0::0,port=%u,family=ipv6", port);
    }
    qcc::String normAnySpec;
    map<qcc::String, qcc::String> normArgMap;
    status = NormalizeListenSpec(anyspec, normAnySpec, normArgMap);
    if (ER_OK != status) {
        QCC_LogError(status, ("TCPTransport::Connect(): Invalid INADDR_ANY connect spec"));
        return status;
    }

    /*
     * Look to see if we are already listening on the provided connectSpec
     * either explicitly or via the INADDR_ANY address.
     */
    QCC_DbgHLPrintf(("DaemonTCPTransport::Connect(): Checking for connection to self"));
    m_listenFdsLock.Lock(MUTEX_CONTEXT);
    bool anyEncountered = false;
    for (list<pair<qcc::String, SocketFd> >::iterator i = m_listenFds.begin(); i != m_listenFds.end(); ++i) {
        QCC_DbgHLPrintf(("DaemonTCPTransport::Connect(): Checking listenSpec %s", i->first.c_str()));

        /*
         * If the provided connectSpec is already explicitly listened to, it is
         * an error.
         */
        if (i->first == normSpec) {
            m_listenFdsLock.Unlock(MUTEX_CONTEXT);
            QCC_DbgHLPrintf(("DaemonTCPTransport::Connect(): Explicit connection to self"));
            return ER_BUS_ALREADY_LISTENING;
        }

        /*
         * If we are listening to INADDR_ANY and the supplied port, then we have
         * to look to the currently UP interfaces to decide if this call is bogus
         * or not.  Set a flag to remind us.
         */
        if (i->first == normAnySpec) {
            QCC_DbgHLPrintf(("DaemonTCPTransport::Connect(): Possible implicit connection to self detected"));
            anyEncountered = true;
        }
    }
    m_listenFdsLock.Unlock(MUTEX_CONTEXT);

    /*
     * If we are listening to INADDR_ANY, we are going to have to see if any
     * currently UP interfaces have an address that matches the connectSpec
     * addr.
     */
    if (anyEncountered) {
        QCC_DbgHLPrintf(("DaemonTCPTransport::Connect(): Checking for implicit connection to self"));
        std::vector<qcc::IfConfigEntry> entries;
        QStatus status = qcc::IfConfig(entries);

        /*
         * Only do the check for self-ness if we can get interfaces to check.
         * This is a non-fatal error since we know that there is an end-to-end
         * check happening in the bus hello exchange, so if there is a problem
         * it will simply be detected later.
         */
        if (status == ER_OK) {
            /*
             * Loop through the network interface entries looking for an UP
             * interface that has the same IP address as the one we're trying to
             * connect to.  We know any match on the address will be a hit since
             * we matched the port during the listener check above.  Since we
             * have a listener listening on *any* UP interface on the specified
             * port, a match on the interface address with the connect address
             * is a hit.
             */
            for (uint32_t i = 0; i < entries.size(); ++i) {
                QCC_DbgHLPrintf(("DaemonTCPTransport::Connect(): Checking interface %s", entries[i].m_name.c_str()));
                if (entries[i].m_flags & qcc::IfConfigEntry::UP) {
                    QCC_DbgHLPrintf(("DaemonTCPTransport::Connect(): Interface UP with addresss %s", entries[i].m_addr.c_str()));
                    IPAddress foundAddr(entries[i].m_addr);
                    if (foundAddr == ipAddr) {
                        QCC_DbgHLPrintf(("DaemonTCPTransport::Connect(): Attempted connection to self; exiting"));
                        return ER_BUS_ALREADY_LISTENING;
                    }
                }
            }
        }
    }

    /*
     * This is a new not previously satisfied connection request, so attempt
     * to connect to the remote TCP address and port specified in the connectSpec.
     */
    SocketFd sockFd = -1;
    status = Socket(family, QCC_SOCK_STREAM, sockFd);
    if (status == ER_OK) {
        /* Turn off Nagle */
        status = SetNagle(sockFd, false);
    }

    if (status == ER_OK) {
        /*
         * We got a socket, now tell TCP to connect to the remote address and
         * port.
         */
        status = qcc::Connect(sockFd, ipAddr, port);
        if (status == ER_OK) {
            /*
             * We now have a TCP connection established, but DBus (the wire
             * protocol which we are using) requires that every connection,
             * irrespective of transport, start with a single zero byte.  This
             * is so that the Unix-domain socket transport used by DBus can pass
             * SCM_RIGHTS out-of-band when that byte is sent.
             */
            uint8_t nul = 0;
            size_t sent;

            status = Send(sockFd, &nul, 1, sent);
            if (status != ER_OK) {
                QCC_LogError(status, ("TCPTransport::Connect(): Failed to send initial NUL byte"));
            }
            isConnected = true;
        } else {
            QCC_LogError(status, ("TCPTransport::Connect(): Failed"));
        }
    } else {
        QCC_LogError(status, ("TCPTransport::Connect(): qcc::Socket() failed"));
    }

    /*
     * The underling transport mechanism is started, but we need to create a
     * TCPEndpoint object that will orchestrate the movement of data across the
     * transport.
     */
    DaemonTCPEndpoint* conn = NULL;
    if (status == ER_OK) {
        conn = new DaemonTCPEndpoint(this, m_bus, false, normSpec, sockFd, ipAddr, port);
        m_endpointListLock.Lock(MUTEX_CONTEXT);
        m_endpointList.push_back(conn);
        m_endpointListLock.Unlock(MUTEX_CONTEXT);

        /* Initialized the features for this endpoint */
        conn->GetFeatures().isBusToBus = true;
        conn->GetFeatures().allowRemote = m_bus.GetInternal().AllowRemoteMessages();
        conn->GetFeatures().handlePassing = false;

        qcc::String authName;
        qcc::String redirection;
        status = conn->Establish("ANONYMOUS", authName, redirection);
        if (status == ER_OK) {
            conn->SetListener(this);
            status = conn->Start();
        }

        /*
         * We put the endpoint into our list of active endpoints to make life
         * easier reporting problems up the chain of command behind the scenes
         * if we got an error during the authentincation process and the endpoint
         * startup.  If we did get an error, we need to remove the endpoint if it
         * is still there and the endpoint exit callback didn't kill it.
         */
        if (status != ER_OK && conn) {
            QCC_LogError(status, ("DaemonTCPTransport::Connect(): Start TCPEndpoint failed"));

            m_endpointListLock.Lock(MUTEX_CONTEXT);
            list<DaemonTCPEndpoint*>::iterator i = find(m_endpointList.begin(), m_endpointList.end(), conn);
            if (i != m_endpointList.end()) {
                m_endpointList.erase(i);
            }
            m_endpointListLock.Unlock(MUTEX_CONTEXT);
            delete conn;
            conn = NULL;
        }
    }

    /*
     * If we got an error, we need to cleanup the socket and zero out the
     * returned endpoint.  If we got this done without a problem, we return
     * a pointer to the new endpoint.
     */
    if (status != ER_OK) {
        if (isConnected) {
            qcc::Shutdown(sockFd);
        }
        if (sockFd >= 0) {
            qcc::Close(sockFd);
        }
        if (newep) {
            *newep = NULL;
        }
    } else {
        if (newep) {
            *newep = conn;
        }
    }

    return status;
}

QStatus DaemonTCPTransport::Disconnect(const char* connectSpec)
{
    QCC_DbgHLPrintf(("DaemonTCPTransport::Disconnect(): %s", connectSpec));

    /*
     * We only want to allow this call to proceed if we have a running server
     * accept thread that isn't in the process of shutting down.  We use the
     * thread response from IsRunning to give us an idea of what our server
     * accept (Run) thread is doing, and by extension the endpoint threads which
     * must be running to properly clean up.  See the comment in Start() for
     * details about what IsRunning actually means, which might be subtly
     * different from your intuitition.
     *
     * If we see IsRunning(), the thread might actually have gotten a Stop(),
     * but has not yet exited its Run routine and become STOPPING.  To plug this
     * hole, we need to check IsRunning() and also m_stopping, which is set in
     * our Stop() method.
     */
    if (IsRunning() == false || m_stopping == true) {
        QCC_LogError(ER_BUS_TRANSPORT_NOT_STARTED, ("DaemonTCPTransport::Disconnect(): Not running or stopping; exiting"));
        return ER_BUS_TRANSPORT_NOT_STARTED;
    }

    /*
     * If we pass the IsRunning() gate above, we must have a server accept
     * thread spinning up or shutting down but not yet joined.  Since the name
     * service is created before the server accept thread is spun up, and
     * deleted after it is joined, we must have a valid name service or someone
     * isn't playing by the rules; so an assert is appropriate here.
     */
    assert(m_ns);

    /*
     * Higher level code tells us which connection is refers to by giving us the
     * same connect spec it used in the Connect() call.  We have to determine the
     * address and port in exactly the same way
     */
    qcc::String normSpec;
    map<qcc::String, qcc::String> argMap;
    QStatus status = NormalizeTransportSpec(connectSpec, normSpec, argMap);
    if (ER_OK != status) {
        QCC_LogError(status, ("DaemonTCPTransport::Disconnect(): Invalid TCP connect spec \"%s\"", connectSpec));
        return status;
    }

    IPAddress ipAddr(argMap.find("addr")->second); // Guaranteed to be there.
    uint16_t port = StringToU32(argMap["port"]);   // Guaranteed to be there.

    /*
     * Stop the remote endpoint.  Be careful here since calling Stop() on the
     * TCPEndpoint is going to cause the transmit and receive threads of the
     * underlying RemoteEndpoint to exit, which will cause our EndpointExit()
     * to be called, which will walk the list of endpoints and delete the one
     * we are stopping.  Once we poke ep->Stop(), the pointer to ep must be
     * considered dead.
     */
    status = ER_BUS_BAD_TRANSPORT_ARGS;
    m_endpointListLock.Lock(MUTEX_CONTEXT);
    for (list<DaemonTCPEndpoint*>::iterator i = m_endpointList.begin(); i != m_endpointList.end(); ++i) {
        if ((*i)->GetPort() == port && (*i)->GetIPAddress() == ipAddr) {
            DaemonTCPEndpoint* ep = *i;
            ep->SetSuddenDisconnect(false);
            m_endpointListLock.Unlock(MUTEX_CONTEXT);
            return ep->Stop();
        }
    }
    m_endpointListLock.Unlock(MUTEX_CONTEXT);
    return status;
}

QStatus DaemonTCPTransport::StartListen(const char* listenSpec)
{
    QCC_DbgPrintf(("DaemonTCPTransport::StartListen()"));
    /*
     * We only want to allow this call to proceed if we have a running server
     * accept thread that isn't in the process of shutting down.  We use the
     * thread response from IsRunning to give us an idea of what our server
     * accept (Run) thread is doing.  See the comment in Start() for details
     * about what IsRunning actually means, which might be subtly different from
     * your intuitition.
     *
     * If we see IsRunning(), the thread might actually have gotten a Stop(),
     * but has not yet exited its Run routine and become STOPPING.  To plug this
     * hole, we need to check IsRunning() and also m_stopping, which is set in
     * our Stop() method.
     */
    if (IsRunning() == false || m_stopping == true) {
        QCC_LogError(ER_BUS_TRANSPORT_NOT_STARTED, ("DaemonTCPTransport::StartListen(): Not running or stopping; exiting"));
        return ER_BUS_TRANSPORT_NOT_STARTED;
    }

    /*
     * If we pass the IsRunning() gate above, we must have a server accept
     * thread spinning up or shutting down but not yet joined.  Since the name
     * service is created before the server accept thread is spun up, and
     * deleted after it is joined, we must have a valid name service or someone
     * isn't playing by the rules; so an assert is appropriate here.
     */
    assert(m_ns);

    /*
     * Normalize the listen spec.  Although this looks like a connectSpec it is
     * different in that reasonable defaults are possible.  We do the
     * normalization here so we can report an error back to the caller.
     */
    qcc::String normSpec;
    map<qcc::String, qcc::String> argMap;
    QStatus status = NormalizeListenSpec(listenSpec, normSpec, argMap);
    if (status != ER_OK) {
        QCC_LogError(status, ("DaemonTCPTransport::StartListen(): Invalid TCP listen spec \"%s\"", listenSpec));
        return status;
    }

    QCC_DbgPrintf(("DaemonTCPTransport::StartListen(): addr = \"%s\", port = \"%s\", family=\"%s\"",
                   argMap["addr"].c_str(), argMap["port"].c_str(), argMap["family"].c_str()));

    /*
     * Because we are sending a *request* to start listening on a given
     * normalized listen spec to another thread, and the server thread starts
     * and stops listening on given listen specs when it decides to eventually
     * run, it is be possible for a calling thread to send multiple requests to
     * start or stop listening on the same listenSpec before the server thread
     * responds.
     *
     * In order to deal with these two timelines, we keep a list of normalized
     * listenSpecs that we have requested to be started, and not yet requested
     * to be removed.  This list (the mListenSpecs) must be consistent with
     * client requests to start and stop listens.  This list is not necessarily
     * consistent with what is actually being listened on.  That is a separate
     * list called mListenFds.
     *
     * So, check to see if someone has previously requested that the address and
     * port in question be listened on.  We need to do this here to be able to
     * report an error back to the caller.
     */
    m_listenSpecsLock.Lock(MUTEX_CONTEXT);
    for (list<qcc::String>::iterator i = m_listenSpecs.begin(); i != m_listenSpecs.end(); ++i) {
        if (*i == normSpec) {
            m_listenSpecsLock.Unlock(MUTEX_CONTEXT);
            return ER_BUS_ALREADY_LISTENING;
        }
    }
    m_listenSpecsLock.Unlock(MUTEX_CONTEXT);

    QueueStartListen(normSpec);
    return ER_OK;
}

void DaemonTCPTransport::QueueStartListen(qcc::String& normSpec)
{
    QCC_DbgPrintf(("DaemonTCPTransport::QueueStartListen()"));

    /*
     * In order to start a listen, we send the server accept thread a message
     * containing the start request code ADD and the normalized listen spec
     * which specifies the address and port to listen on.
     */
    ListenRequest listenRequest;
    listenRequest.m_request = START_LISTEN;
    listenRequest.m_listenSpec = normSpec;

    m_listenRequestsLock.Lock(MUTEX_CONTEXT);
    m_listenRequests.push(listenRequest);
    m_listenRequestsLock.Unlock(MUTEX_CONTEXT);

    /*
     * Wake the server accept loop thread up so it will process the request we
     * just queued.
     */
    Alert();
}

void DaemonTCPTransport::DoStartListen(qcc::String& normSpec)
{
    QCC_DbgPrintf(("DaemonTCPTransport::DoStartListen()"));

    /*
     * Since the name service is created before the server accept thread is spun
     * up, and deleted after it is joined, we must have a valid name service or
     * someone isn't playing by the rules; so an assert is appropriate here.
     */
    assert(m_ns);

    /*
     * Parse the normalized listen spec.  The easiest way to do this is to
     * re-normalize it.  If there's an error at this point, we have done
     * something wrong since the listen spec was presumably successfully
     * normalized before sending it in -- so we assert.
     */
    qcc::String spec;
    map<qcc::String, qcc::String> argMap;
    QStatus status = NormalizeListenSpec(normSpec.c_str(), spec, argMap);
    assert(status == ER_OK && "DaemonTCPTransport::DoStartListen(): Invalid TCP listen spec");

    QCC_DbgPrintf(("DaemonTCPTransport::DoStartListen(): addr = \"%s\", port = \"%s\", family=\"%s\"",
                   argMap["addr"].c_str(), argMap["port"].c_str(), argMap["family"].c_str()));

    m_listenFdsLock.Lock(MUTEX_CONTEXT);

    /*
     * Figure out what local address and port the listener should use.
     */
    IPAddress listenAddr(argMap["addr"]);
    uint16_t listenPort = StringToU32(argMap["port"]);
    qcc::AddressFamily family = argMap["family"] == "ipv6" ?  QCC_AF_INET6 : QCC_AF_INET;

    /*
     * If we're going to listen on an address, we are going to listen on a
     * corresponding network interface.  We need to convince the name service to
     * send advertisements out over that interface, or nobody will know to
     * connect to the listening daemon.  The expected use case is that the
     * daemon does exactly one StartListen() which listens to INADDR_ANY
     * (listens for inbound connections over any interface) and the name service
     * is controlled by a separate configuration item that selects which
     * interfaces are used in discovery.  Since IP addresses in a mobile
     * environment are dynamic, listening on the ANY address is the only option
     * that really makes sense, and this is the only case in which the current
     * implementation will really work.
     *
     * So, we need to get the configuration item telling us which network
     * interfaces we should run the name service over.  The item can specify an
     * IP address, in which case the name service waits until that particular
     * address comes up and then uses the corresponding net device if it is
     * multicast-capable.  The item can also specify an interface name.  In this
     * case the name service waits until it finds the interface IFF_UP and
     * multicast capable with an assigned IP address and then starts using the
     * interface.  If the configuration item contains "*" (the wildcard) it is
     * interpreted as meaning all multicast-capable interfaces.  If the
     * configuration item is empty (not assigned in the configuration database)
     * it defaults to "*".
     */
    qcc::String interfaces = ConfigDB::GetConfigDB()->GetProperty(NameService::MODULE_NAME, NameService::INTERFACES_PROPERTY);
    if (interfaces.size() == 0) {
        interfaces = INTERFACES_DEFAULT;
    }

    while (interfaces.size()) {
        qcc::String currentInterface;
        size_t i = interfaces.find(",");
        if (i != qcc::String::npos) {
            currentInterface = interfaces.substr(0, i);
            interfaces = interfaces.substr(i + 1, interfaces.size() - i - 1);
        } else {
            currentInterface = interfaces;
            interfaces.clear();
        }
        /*
         * If we were given and IP address use it to find the interface names
         * otherwise use the interface name that was specified. Note we need
         * to disallow hostnames otherwise SetAddress will attempt to treat
         * the interface name as a host name and start doing DNS lookups.
         */
        assert(m_ns);
        IPAddress currentAddress;
        if (currentAddress.SetAddress(currentInterface, false) == ER_OK) {
            status = m_ns->OpenInterface(currentAddress);
        } else {
            status = m_ns->OpenInterface(currentInterface);
        }
        if (status != ER_OK) {
            QCC_LogError(status, ("DaemonTCPTransport::DoStartListen(): OpenInterface() failed for %s", currentInterface.c_str()));
        }
    }

    /*
     * We have the name service work out of the way, so we can now create the
     * TCP listener sockets and set SO_REUSEADDR/SO_REUSEPORT so we don't have
     * to wait for four minutes to relaunch the daemon if it crashes.
     */
    SocketFd listenFd = -1;
    status = Socket(family, QCC_SOCK_STREAM, listenFd);
    if (status != ER_OK) {
        m_listenFdsLock.Unlock(MUTEX_CONTEXT);
        QCC_LogError(status, ("DaemonTCPTransport::DoStartListen(): Socket() failed"));
        return;
    }

    /*
     * Set the SO_REUSEADDR socket option so we don't have to wait for four
     * minutes while the endponit is in TIME_WAIT if we crash (or control-C).
     */
    status = qcc::SetReuseAddress(listenFd, true);
    if (status != ER_OK) {
        QCC_LogError(status, ("DaemonTCPTransport::DoStartListen(): SetReuseAddress() failed"));
        qcc::Close(listenFd);
        return;
    }

    /*
     * Bind the socket to the listen address and start listening for incoming
     * connections on it.
     */
    status = Bind(listenFd, listenAddr, listenPort);
    if (status == ER_OK) {
        /*
         * On Android, the bundled daemon will not set the TCP port in the listen
         * spec so as to let the kernel to find an unused port for the TCP
         * transport; thus call GetLocalAddress() to get the actual TCP port
         * used after Bind() and update the connect spec here.
         */
        qcc::GetLocalAddress(listenFd, listenAddr, listenPort);
        normSpec = "tcp:addr=" + argMap["addr"] + ",port=" + U32ToString(listenPort);

        status = qcc::Listen(listenFd, SOMAXCONN);
        if (status == ER_OK) {
            QCC_DbgPrintf(("DaemonTCPTransport::DoStartListen(): Listening on %s/%d", argMap["addr"].c_str(), listenPort));
            m_listenFds.push_back(pair<qcc::String, SocketFd>(normSpec, listenFd));
        } else {
            QCC_LogError(status, ("DaemonTCPTransport::DoStartListen(): Listen failed"));
        }
    } else {
        QCC_LogError(status, ("DaemonTCPTransport::DoStartListen(): Failed to bind to %s/%d", listenAddr.ToString().c_str(), listenPort));
    }

    /*
     * The name service is very flexible about what to advertise.  Empty
     * strings tell the name service to use IP addreses discovered from
     * addresses returned in socket receive calls.  Providing explicit IPv4
     * or IPv6 addresses trumps this and allows us to advertise one interface
     * over a name service running on another.  The name service allows
     * this, but we don't use the feature.
     *
     * N.B. This means that if we listen on a specific IP address and advertise
     * over other interfaces chosen by the name service (which do not have that
     * specific IP address assigned) we can end up advertising services on IP
     * addresses that are not present on the network that gets the
     * advertisements.
     *
     * Another thing to understand is that there is one name service per
     * instance of DaemonTCPTransport, and the name service allows only one
     * combination of IPv4 address, IPv6 address and port -- it uses the last
     * one set.  If no addresses are provided, the name service advertises the
     * IP address of each of the interfaces it chooses using the last provided
     * port.  Each call to SetEndpoints() below will then overrwite the
     * advertised daemon listen port.  It is not currently possible to have
     * a daemon listening on multiple TCP ports.
     */
    assert(m_ns);
    m_ns->SetEndpoints("", "", listenPort);
    m_listenFdsLock.Unlock(MUTEX_CONTEXT);

    /*
     * Signal the (probably) waiting run thread so it will wake up and add this
     * new socket to its list of sockets it is waiting for connections on.
     */
    if (status == ER_OK) {
        Alert();
    }
}

QStatus DaemonTCPTransport::StopListen(const char* listenSpec)
{
    QCC_DbgPrintf(("DaemonTCPTransport::StopListen()"));

    /*
     * We only want to allow this call to proceed if we have a running server
     * accept thread that isn't in the process of shutting down.  We use the
     * thread response from IsRunning to give us an idea of what our server
     * accept (Run) thread is doing.  See the comment in Start() for details
     * about what IsRunning actually means, which might be subtly different from
     * your intuitition.
     *
     * If we see IsRunning(), the thread might actually have gotten a Stop(),
     * but has not yet exited its Run routine and become STOPPING.  To plug this
     * hole, we need to check IsRunning() and also m_stopping, which is set in
     * our Stop() method.
     */
    if (IsRunning() == false || m_stopping == true) {
        QCC_LogError(ER_BUS_TRANSPORT_NOT_STARTED, ("DaemonTCPTransport::StopListen(): Not running or stopping; exiting"));
        return ER_BUS_TRANSPORT_NOT_STARTED;
    }

    /*
     * If we pass the IsRunning() gate above, we must have a server accept
     * thread spinning up or shutting down but not yet joined.  Since the name
     * service is created before the server accept thread is spun up, and
     * deleted after it is joined, we must have a valid name service or someone
     * isn't playing by the rules; so an assert is appropriate here.
     */
    assert(m_ns);

    /*
     * Normalize the listen spec.  We are going to use the name string that was
     * put together for the StartListen call to find the listener instance to
     * stop, so we need to do it exactly the same way.
     */
    qcc::String normSpec;
    map<qcc::String, qcc::String> argMap;
    QStatus status = NormalizeListenSpec(listenSpec, normSpec, argMap);
    if (status != ER_OK) {
        QCC_LogError(status, ("DaemonTCPTransport::StopListen(): Invalid TCP listen spec \"%s\"", listenSpec));
        return status;
    }

    /*
     * Because we are sending a *request* to stop listening on a given
     * normalized listen spec to another thread, and the server thread starts
     * and stops listening on given listen specs when it decides to eventually
     * run, it is be possible for a calling thread to send multiple requests to
     * start or stop listening on the same listenSpec before the server thread
     * responds.
     *
     * In order to deal with these two timelines, we keep a list of normalized
     * listenSpecs that we have requested to be started, and not yet requested
     * to be removed.  This list (the mListenSpecs) must be consistent with
     * client requests to start and stop listens.  This list is not necessarily
     * consistent with what is actually being listened on.  That is reflected by
     * a separate list called mListenFds.
     *
     * We consult the list of listen spects for duplicates when starting to
     * listen, and we make sure that a listen spec is on the list before
     * queueing a request to stop listening.  Asking to stop listening on a
     * listen spec we aren't listening on is not an error, since the goal of the
     * user is to not listen on a given address and port -- and we aren't.
     */
    m_listenSpecsLock.Lock(MUTEX_CONTEXT);
    for (list<qcc::String>::iterator i = m_listenSpecs.begin(); i != m_listenSpecs.end(); ++i) {
        if (*i == normSpec) {
            m_listenSpecs.erase(i);
            QueueStopListen(normSpec);
            break;
        }
    }
    m_listenSpecsLock.Unlock(MUTEX_CONTEXT);

    return ER_OK;
}

void DaemonTCPTransport::QueueStopListen(qcc::String& normSpec)
{
    QCC_DbgPrintf(("DaemonTCPTransport::QueueStopListen()"));

    /*
     * In order to start a listen, we send the server accept thread a message
     * containing the start request code ADD and the normalized listen spec
     * which specifies the address and port to listen on.
     */
    ListenRequest listenRequest;
    listenRequest.m_request = STOP_LISTEN;
    listenRequest.m_listenSpec = normSpec;

    m_listenRequestsLock.Lock(MUTEX_CONTEXT);
    m_listenRequests.push(listenRequest);
    m_listenRequestsLock.Unlock(MUTEX_CONTEXT);

    /*
     * Wake the server accept loop thread up so it will process the request we
     * just queued.
     */
    Alert();
}

void DaemonTCPTransport::DoStopListen(qcc::String& normSpec)
{
    QCC_DbgPrintf(("DaemonTCPTransport::DoStopListen()"));

    /*
     * Since the name service is created before the server accept thread is spun
     * up, and deleted after it is joined, we must have a valid name service or
     * someone isn't playing by the rules; so an assert is appropriate here.
     */
    assert(m_ns);

    /*
     * Find the (single) listen spec and remove it from the list of active FDs
     * used by the server accept loop (run thread).  This is okay to do since
     * we are assuming that, since we should only be called in the context of
     * the server accept loop, it knows that an FD will be deleted here.
     */
    m_listenFdsLock.Lock(MUTEX_CONTEXT);
    qcc::SocketFd stopFd = -1;
    bool found = false;
    for (list<pair<qcc::String, SocketFd> >::iterator i = m_listenFds.begin(); i != m_listenFds.end(); ++i) {
        if (i->first == normSpec) {
            stopFd = i->second;
            m_listenFds.erase(i);
            found = true;
            break;
        }
    }
    m_listenFdsLock.Unlock(MUTEX_CONTEXT);

    /*
     * If we took a socketFD off of the list of active FDs, we need to tear it
     * down and alert the server accept loop that the list of FDs on which it
     * is listening has changed.
     */
    if (found) {
        qcc::Shutdown(stopFd);
        qcc::Close(stopFd);
    }
}

void DaemonTCPTransport::EnableDiscovery(const char* namePrefix)
{
    /*
     * We only want to allow this call to proceed if we have a running server
     * accept thread that isn't in the process of shutting down.  We use the
     * thread response from IsRunning to give us an idea of what our server
     * accept (Run) thread is doing.  See the comment in Start() for details
     * about what IsRunning actually means, which might be subtly different from
     * your intuitition.
     *
     * If we see IsRunning(), the thread might actually have gotten a Stop(),
     * but has not yet exited its Run routine and become STOPPING.  To plug this
     * hole, we need to check IsRunning() and also m_stopping, which is set in
     * our Stop() method.
     */
    if (IsRunning() == false || m_stopping == true) {
        QCC_LogError(ER_BUS_TRANSPORT_NOT_STARTED, ("DaemonTCPTransport::EnableDiscovery(): Not running or stopping; exiting"));
        return;
    }

    /*
     * If we pass the IsRunning() gate above, we must have a server accept
     * thread spinning up or shutting down but not yet joined.  Since the name
     * service is created before the server accept thread is spun up, and
     * deleted after it is joined, we must have a valid name service or someone
     * isn't playing by the rules; so an assert is appropriate here.
     */
    assert(m_ns);

    /*
     * When a bus name is advertised, the source may append a string that
     * identifies a specific instance of advertised name.  For example, one
     * might advertise something like
     *
     *   com.mycompany.myproduct.0123456789ABCDEF
     *
     * as a specific instance of the bus name,
     *
     *   com.mycompany.myproduct
     *
     * Clients of the system will want to be able to discover all specific
     * instances, so they need to do a wildcard search for bus name strings
     * that match the non-specific name, for example,
     *
     *   com.mycompany.myproduct*
     *
     * We automatically append the name service wildcard character to the end
     * of the provided string (which we call the namePrefix) before sending it
     * to the name service which forwards the request out over the net.
     */
    String starPrefix = namePrefix;
    starPrefix.append('*');

    QStatus status = m_ns->Locate(starPrefix);
    if (status != ER_OK) {
        QCC_LogError(status, ("DaemonTCPTransport::EnableDiscovery(): Failure on \"%s\"", namePrefix));
    }
}

QStatus DaemonTCPTransport::EnableAdvertisement(const qcc::String& advertiseName)
{
    /*
     * We only want to allow this call to proceed if we have a running server
     * accept thread that isn't in the process of shutting down.  We use the
     * thread response from IsRunning to give us an idea of what our server
     * accept (Run) thread is doing.  See the comment in Start() for details
     * about what IsRunning actually means, which might be subtly different from
     * your intuitition.
     *
     * If we see IsRunning(), the thread might actually have gotten a Stop(),
     * but has not yet exited its Run routine and become STOPPING.  To plug this
     * hole, we need to check IsRunning() and also m_stopping, which is set in
     * our Stop() method.
     */
    if (IsRunning() == false || m_stopping == true) {
        QCC_LogError(ER_BUS_TRANSPORT_NOT_STARTED, ("DaemonTCPTransport::EnableAdvertisement(): Not running or stopping; exiting"));
        return ER_BUS_TRANSPORT_NOT_STARTED;
    }

    /*
     * If we pass the IsRunning() gate above, we must have a server accept
     * thread spinning up or shutting down but not yet joined.  Since the name
     * service is created before the server accept thread is spun up, and
     * deleted after it is joined, we must have a valid name service or someone
     * isn't playing by the rules; so an assert is appropriate here.
     */
    assert(m_ns);

    /*
     * Give the provided name to the name service and have it start advertising
     * the name on the network as reachable through the daemon having this
     * transport.  The name service handles periodic retransmission of the name
     * and manages the coming and going of network interfaces for us.
     */
    QStatus status = m_ns->Advertise(advertiseName);
    if (status != ER_OK) {
        QCC_LogError(status, ("DaemonTCPTransport::EnableAdvertisment(): Failure on \"%s\"", advertiseName.c_str()));
    }
    return status;
}

void DaemonTCPTransport::DisableAdvertisement(const qcc::String& advertiseName, bool nameListEmpty)
{
    /*
     * We only want to allow this call to proceed if we have a running server
     * accept thread that isn't in the process of shutting down.  We use the
     * thread response from IsRunning to give us an idea of what our server
     * accept (Run) thread is doing.  See the comment in Start() for details
     * about what IsRunning actually means, which might be subtly different from
     * your intuitition.
     *
     * If we see IsRunning(), the thread might actually have gotten a Stop(),
     * but has not yet exited its Run routine and become STOPPING.  To plug this
     * hole, we need to check IsRunning() and also m_stopping, which is set in
     * our Stop() method.
     */
    if (IsRunning() == false || m_stopping == true) {
        QCC_LogError(ER_BUS_TRANSPORT_NOT_STARTED, ("DaemonTCPTransport::DisableAdvertisement(): Not running or stopping; exiting"));
        return;
    }

    /*
     * If we pass the IsRunning() gate above, we must have a server accept
     * thread spinning up or shutting down but not yet joined.  Since the name
     * service is created before the server accept thread is spun up, and
     * deleted after it is joined, we must have a valid name service or someone
     * isn't playing by the rules; so an assert is appropriate here.
     */
    assert(m_ns);

    /*
     * Tell the name service to stop advertising the provided name on the
     * network as reachable through the daemon having this transport.  The name
     * service sends out a no-longer-here message and stops periodic
     * retransmission of the name as a result of the Cancel() call.
     */
    QStatus status = m_ns->Cancel(advertiseName);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failure stop advertising \"%s\" for TCP", advertiseName.c_str()));
    }
}

void DaemonTCPTransport::FoundCallback::Found(const qcc::String& busAddr, const qcc::String& guid,
                                              std::vector<qcc::String>& nameList, uint8_t timer)
{
    /*
     * Whenever the name service receives a message indicating that a bus-name
     * is out on the network somewhere, it sends a message back to us via this
     * callback.  In order to avoid duplication of effort, the name service does
     * not manage a cache of names, but delegates that to the daemon having this
     * transport.  If the timer parameter is non-zero, it indicates that the
     * nameList (actually a vector of bus-name Strings) can be expected to be
     * valid for the value of timer in seconds.  If timer is zero, it means that
     * the bus names in the nameList are no longer available and should be
     * flushed out of the daemon name cache.
     *
     * The name service does not have a cache and therefore cannot time out
     * entries, but also delegates that task to the daemon.  It is expected that
     * remote daemons will send keepalive messages that the local daemon will
     * recieve, also via this callback.
     *
     * Our job here is just to pass the messages on up the stack to the daemon.
     */
    if (m_listener) {
        m_listener->FoundNames(busAddr, guid, TRANSPORT_WLAN, &nameList, timer);
    }
}

} // namespace ajn
