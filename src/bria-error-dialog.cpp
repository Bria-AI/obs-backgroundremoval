// SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bria-error-dialog.h"
#include "bria-analytics.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QDialog>
#include <QLabel>
#include <QMainWindow>
#include <QRegularExpression>
#include <QString>
#include <QVBoxLayout>

namespace {

// Escapes plain text for safe display as rich text, then turns any http(s)
// URL into a clickable link (trailing punctuation like ":" or "." is kept
// outside the link so it doesn't get swept into the clickable area/target).
QString toHtmlWithLinks(const QString &plain)
{
	QString html = plain.toHtmlEscaped();
	static const QRegularExpression urlPattern(QStringLiteral(R"((https?://[^\s<]+?)([.,;:!?)]*)(?=\s|$))"));
	html.replace(urlPattern, QStringLiteral(R"(<a href="\1">\1</a>\2)"));
	return html;
}

} // namespace

extern "C" void bria_show_error_dialog(BriaCloseReason reason, const std::string &serverMessage)
{
	const char *msgKey = nullptr;
	switch (reason) {
	case BriaCloseReason::Unauthorized:
		msgKey = "BriaErrorUnauthorizedMessage";
		break;
	case BriaCloseReason::GeneralError:
		msgKey = "BriaErrorGeneralMessage";
		break;
	case BriaCloseReason::SessionLimitReached:
		msgKey = "BriaErrorSessionLimitMessage";
		break;
	case BriaCloseReason::CapacityExceeded:
		msgKey = "BriaErrorCapacityMessage";
		break;
	case BriaCloseReason::SessionTimeout:
		msgKey = "BriaErrorTimeoutMessage";
		break;
	case BriaCloseReason::Unknown:
	default:
		return;
	}

	// Prefer the server's own detailed message (e.g. a specific
	// quota-exceeded explanation) over our generic per-reason text.
	const std::string text = serverMessage.empty() ? obs_module_text(msgKey) : serverMessage;

	QMainWindow *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());

	QDialog dialog(main);
	dialog.setFixedWidth(380);
	dialog.setModal(true);
	dialog.setWindowTitle(obs_module_text("BriaErrorDialogTitle"));

	QVBoxLayout *root = new QVBoxLayout(&dialog);
	root->setContentsMargins(28, 24, 28, 24);

	QLabel *message = new QLabel(toHtmlWithLinks(QString::fromStdString(text)));
	message->setTextFormat(Qt::RichText);
	message->setOpenExternalLinks(true);
	message->setWordWrap(true);
	root->addWidget(message);

	// A freshly constructed QDialog has no meaningful position of its own on
	// Windows and otherwise lands near the top-left of the screen; centre it
	// over the OBS main window instead.
	if (main) {
		dialog.adjustSize();
		const QPoint center = main->geometry().center();
		dialog.move(center.x() - dialog.width() / 2, center.y() - dialog.height() / 2);
	}

	BriaAnalytics::instance().capture("obs_plugin_error_shown",
					  {{"reason", std::to_string(static_cast<int>(reason))},
					   {"has_server_message", serverMessage.empty() ? "false" : "true"}});

	dialog.exec();
}
