#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QDialog>
#include <QMap>
#include <QString>
#include <QUrl>
#include <QVector>

namespace Ui
{
  class Updater;
}

namespace Noggit
{
	namespace Ui
    {
        // Noggit Cinder self-updater.
        //
        // On construction it silently fetches "<url>/MD5" (a manifest of "relative/path md5hex"
        // lines), hashes ONLY the locally-present files named in that manifest (user data like
        // project folders is never walked or hashed), and emits OpenUpdater() when anything
        // differs. "Update" downloads the changed files into ./temp and applies them via a
        // generated cmd script that waits for noggit to exit, moves the files over the install
        // and restarts it (the original flow relied on a separate noggit-updater.exe that was
        // never shipped with this fork).
        //
        // The update server URL comes from the QSettings key "updater/url" ("%1" is replaced
        // with the requested file name); "updater/enabled" (default true) is the kill switch.
        // Every network failure is silent: no server or no internet must never bother the user.
        class CUpdater : public QDialog
        {
            Q_OBJECT
            ::Ui::Updater* ui = nullptr;

        public:
            CUpdater(QWidget* parent = nullptr);

        private:
            QByteArray FileMD5(const QString& filename, QCryptographicHash::Algorithm algo);
            QUrl GenerateLink(const QString& name);

            void CompareWithLocal();
            void DownloadUpdate();
            void ApplyUpdateAndRestart();

        private slots:
            void GenerateOnlineMD5();
            void GetOnlineFile();

        signals:
            void OpenUpdater();

        private:
            const QString TemporaryFolder = "/temp";

            // resolved from QSettings "updater/url" in the constructor; default points at the
            // Everwood update server (static file dir behind Caddy).
            QString StorageURL;

            int FileNeededCount = 0;
            int FileMissingCount = 0;

            bool NeedUpdate = false;

            QMap<QString, QString> OnlineMD5;

            QVector<QString> FileNeeded;
        };
    }
}
