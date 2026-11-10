# Official Qt 6 HTTP and SSE sources

## Priority

Read target's actual Qt SDK minor/build and API contracts first. `doc.qt.io/qt-6/` usually displays current Qt 6 and may describe features newer than the target. Distinguish a feature's `Since: Qt 6.x` and later overload introductions.

- QNetworkAccessManager: https://doc.qt.io/qt-6/qnetworkaccessmanager.html
- QNetworkRequest and reply attributes: https://doc.qt.io/qt-6/qnetworkrequest.html
- QNetworkReply: https://doc.qt.io/qt-6/qnetworkreply.html
- Qt 6 Network behavior changes / HTTP/2 default: https://doc.qt.io/qt-6/network-changes-qt6.html
- QNetworkRequestFactory (Qt 6.7+; some members since 6.8): https://doc.qt.io/qt-6/qnetworkrequestfactory.html
- QRestAccessManager (since Qt 6.7): https://doc.qt.io/qt-6/qrestaccessmanager.html
- QRestReply: https://doc.qt.io/qt-6/qrestreply.html
- QTimer: https://doc.qt.io/qt-6/qtimer.html
- QObject thread affinity: https://doc.qt.io/qt-6/threads-qobject.html
- Qt Test: https://doc.qt.io/qt-6/qttest-index.html
- Normative SSE / Last-Event-ID and 204: https://html.spec.whatwg.org/multipage/server-sent-events.html
- HTTP/2 specification: https://www.rfc-editor.org/rfc/rfc9113

Example names and signatures must be checked against the target SDK, not silently upgraded to the latest Qt API.
