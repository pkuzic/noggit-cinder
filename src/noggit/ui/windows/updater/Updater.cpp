#include "ui_Updater.h"
#include "Updater.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QTextStream>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Noggit
{
	namespace Ui
    {
        CUpdater::CUpdater(QWidget* parent) :
            QDialog(parent)
        {
            setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
            ui = new ::Ui::Updater;
            ui->setupUi(this);

            // hide progress
            ui->ProgressFile->hide();
            ui->ProgressDownload->hide();
            hide();

            // connect ui button to update or close
            connect(ui->Close, &QPushButton::clicked, this, [=]() { close(); });
            connect(ui->Update, &QPushButton::clicked, this, [=]() { DownloadUpdate(); });

            QSettings settings;
            if (!settings.value("updater/enabled", true).toBool())
            {
                return;
            }

            StorageURL = settings.value("updater/url",
                "https://updater.everwood.gg/launcher-updates/noggit-cinder/%1").toString();

            QNetworkRequest request(GenerateLink("MD5"));
            QNetworkReply* reply = (new QNetworkAccessManager(this))->get(request);
            connect(reply, &QNetworkReply::finished, this, &CUpdater::GenerateOnlineMD5);
        }

        QByteArray CUpdater::FileMD5(const QString& filename, QCryptographicHash::Algorithm algo)
        {
            QFile file(filename);
            if (file.open(QFile::ReadOnly))
            {
                QCryptographicHash hash(algo);

                if (hash.addData(&file))
                    return hash.result();
            }

            return QByteArray();
        }

        QUrl CUpdater::GenerateLink(const QString& name)
        {
            return QUrl(StorageURL.arg(name));
        }

        void CUpdater::GenerateOnlineMD5()
        {
            QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
            if (!reply)
                return;

            reply->deleteLater();

            // silent on ANY failure: an unreachable server or missing manifest must never
            // bother the user -- noggit simply runs without updates.
            if (reply->error() != QNetworkReply::NoError)
                return;

            for (QByteArray const& raw : reply->readAll().split('\n'))
            {
                QString const line = QString::fromUtf8(raw).trimmed();
                if (line.isEmpty())
                    continue;

                int const sep = line.lastIndexOf(' ');
                if (sep <= 0)
                    continue;

                OnlineMD5[line.left(sep).trimmed()] = line.mid(sep + 1).trimmed();
            }

            CompareWithLocal();
        }

        void CUpdater::CompareWithLocal()
        {
            // manifest-driven: hash only the files the manifest names. NEVER walk the install
            // directory -- it holds the user's multi-gigabyte project folders.
            for (const auto& e : OnlineMD5.toStdMap())
            {
                QString const local_path = QDir::currentPath() + "/" + e.first;
                QString const local_hash = QFile::exists(local_path)
                    ? QString(FileMD5(local_path, QCryptographicHash::Md5).toHex())
                    : QString();

                if (local_hash != e.second)
                {
                    FileNeeded.push_back(e.first);

                    if (ui->FileList->toPlainText().isEmpty())
                    {
                        ui->FileList->append(QString(tr("Files to update:")));
                    }

                    ui->FileList->append(QString(" - %1").arg(e.first));
                }
            }

            if (FileNeeded.isEmpty())
            {
                NeedUpdate = false;
                return;
            }

            FileNeededCount = FileNeeded.size();
            NeedUpdate = true;

            ui->ProgressFile->setMaximum(FileNeededCount);
            ui->ProgressFile->setFormat(QString(tr("File %v/%1")).arg(FileNeededCount));
            ui->ProgressFile->show();

            emit OpenUpdater();
        }

        void CUpdater::DownloadUpdate()
        {
            if (FileNeeded.size() == 0)
                return;

            ui->ProgressDownload->show();

            QNetworkRequest request(GenerateLink(FileNeeded[0]));
            QNetworkReply* reply = (new QNetworkAccessManager(this))->get(request);
            connect(reply, &QNetworkReply::downloadProgress, reply, [this](qint64 received, qint64 total)
                {
                    ui->ProgressDownload->setMaximum(total);
                    ui->ProgressDownload->setValue(received);
                });

            connect(reply, &QNetworkReply::finished, this, &CUpdater::GetOnlineFile);

            ui->FileList->append(QString(tr("Downloading : %1")).arg(FileNeeded[0]));
        }

        void CUpdater::GetOnlineFile()
        {
            QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());

            ui->ProgressFile->setValue(ui->ProgressFile->value() + 1);

            if (reply)
                reply->deleteLater();

            if (!reply || reply->error() != QNetworkReply::NoError)
            {
                FileMissingCount += 1;
                FileNeeded.removeAt(0);
                if (FileNeeded.size() > 0)
                {
                    DownloadUpdate();
                    return;
                }

                ApplyUpdateAndRestart();
                return;
            }

            QDir dir;
            QString const temp_root = QDir::currentPath() + TemporaryFolder;
            if (!dir.exists(temp_root))
                dir.mkpath(temp_root);

            auto index = FileNeeded[0].lastIndexOf('/');
            if (index >= 0)
            {
                dir.mkpath(temp_root + "/" + FileNeeded[0].left(index));
            }

            QFile file(temp_root + "/" + FileNeeded[0]);
            if (file.open(QIODevice::WriteOnly))
            {
                file.write(reply->readAll());
                file.flush();
                file.close();
            }

            FileNeeded.removeAt(0);
            if (FileNeeded.size() > 0)
            {
                DownloadUpdate();
                return;
            }

            ApplyUpdateAndRestart();
        }

        void CUpdater::ApplyUpdateAndRestart()
        {
            if (!NeedUpdate)
                return;

            if (FileNeededCount == FileMissingCount)
                return;

            // noggit.exe is locked while we run. Write a script that waits for the lock to
            // clear (a same-name rename only succeeds once the process is gone), moves the
            // downloaded files over the install, restarts noggit and deletes itself.
            QString const script_path = QDir::currentPath() + "/apply_update.cmd";
            QFile script(script_path);
            if (!script.open(QIODevice::WriteOnly | QIODevice::Text))
                return;

            {
                QTextStream ts(&script);
                ts << "@echo off\n";
                ts << ":wait\n";
                ts << "timeout /t 1 /nobreak >nul\n";
                ts << "ren \"%~dp0noggit.exe\" noggit.exe 2>nul || goto wait\n";
                ts << "robocopy \"%~dp0temp\" \"%~dp0.\" /E /MOVE /NFL /NDL /NJH /NJS >nul\n";
                ts << "rd /s /q \"%~dp0temp\" 2>nul\n";
                ts << "start \"\" \"%~dp0noggit.exe\"\n";
                ts << "del \"%~f0\"\n";
            }
            script.close();

            QProcess::startDetached("cmd.exe", { "/c", script_path });
            QCoreApplication::quit();
        }
    }
}
