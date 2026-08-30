// Timer-lifecycle regression for LpMeterConnection's auto-reconnect.
//
// Disabling auto-reconnect must also CANCEL a retry already counting down.
// Before #5320's review, setAutoReconnect() only flipped the flag and the
// armed QTimer's callback never re-read it, so an operator who switched the
// option off during the five-second delay still got one more reconnect.
//
// Deliberately narrow: this exercises the timer's lifecycle through the
// public API and nothing else. It needs a connection FAILURE to arm the
// timer, which it gets from a loopback port nothing listens on -- refused
// immediately by the kernel, no external network, no hardware. If the
// environment refuses even that (a sandbox with no loopback), the test
// SKIPS rather than failing, because a flaky red is worse than a gap.

#include "core/LpMeterConnection.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>

namespace {

int g_failed = 0;

void report(const char* what, bool ok)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) {
        ++g_failed;
    }
}

// Port 1 on loopback: privileged, and nothing binds it in any CI image we
// run. A connect() there is refused by the kernel without leaving the host.
constexpr quint16 kRefusedPort = 1;
constexpr int kFailWaitMs = 5000;

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    AetherSDR::LpMeterConnection conn;
    conn.setAutoReconnect(true);

    // Wait for the connection to fail, which is what arms the retry.
    QEventLoop loop;
    bool failed = false;
    QObject::connect(&conn, &AetherSDR::LpMeterConnection::connectionFailed,
                     &loop, [&](const QString&) { failed = true; loop.quit(); });
    QTimer::singleShot(kFailWaitMs, &loop, &QEventLoop::quit);
    conn.connectNetwork(QStringLiteral("127.0.0.1"), kRefusedPort);
    loop.exec();

    if (!failed) {
        std::printf("SKIP: loopback connect to port %u neither connected nor "
                    "was refused within %d ms; this environment cannot arm "
                    "the retry, so the lifecycle is untestable here.\n",
                    kRefusedPort, kFailWaitMs);
        return 77;   // ctest SKIP_RETURN_CODE
    }

    report("a failed connect arms a retry while auto-reconnect is on",
           conn.reconnectPending());

    // The regression itself.
    conn.setAutoReconnect(false);
    report("disabling auto-reconnect CANCELS the armed retry",
           !conn.reconnectPending());

    // And re-enabling must not resurrect the cancelled one.
    conn.setAutoReconnect(true);
    report("re-enabling does not resurrect a cancelled retry",
           !conn.reconnectPending());

    // A deliberate disconnect must not arm one either.
    conn.disconnect();
    report("a deliberate disconnect leaves no retry armed",
           !conn.reconnectPending());

    std::printf("\n%s\n", g_failed == 0 ? "all passed" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
