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
        class CUpdater : public QDialog
        {
            Q_OBJECT
            ::Ui::Updater* ui = nullptr;

        public:
            CUpdater(QWidget* parent = nullptr);

        private:
            QByteArray FileMD5(const QString& filename, QCryptographicHash::Algorithm algo);
            QString ToHashFile(const QString& name, const QString& hash);
            QUrl GenerateLink(const QString& name);

            void GenerateLocalMD5();
            void CompareMD5();
            void DownloadUpdate();
            void StartExternalUpdater();

        private slots:
            void GenerateOnlineMD5();
            void GetOnlineFile();

        signals:
            void OpenUpdater();

        private:
            const QString TemporaryFolder = "/temp";
            // DISARMED for Noggit Cinder: this used to point at the upstream Noggit RED binaries
            // repo -- running the updater would have overwritten this fork's exe with an upstream
            // build (no 1.12 support). The updater is already never instantiated (commented out in
            // NoggitProjectSelectionWindow), and this URL now points at the Cinder repo, which
            // hosts no binary manifest -- so even a re-enabled updater cannot fetch anything.
            const QString StorageURL = "https://raw.githubusercontent.com/pkuzic/noggit-cinder/main/%1";
            const QString FileURL = "%1%2/%3";
            const QString ExternalProcess = "/noggit-updater.exe";

            int FileNeededCount = 0;
            int FileMissingCount = 0;

            bool NeedUpdate = false;
            bool LocalCheck = false, OnlineCheck = false;

            QMap<QString, QString> LocalMD5;
            QMap<QString, QString> OnlineMD5;

            QVector<QString> FileNeeded;
        };
    }
}
