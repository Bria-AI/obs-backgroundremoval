// SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bria-welcome-dialog.h"
#include "bria-analytics.hpp"

#include <obs-frontend-api.h>

#include <QDesktopServices>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

static const char *SETUP_GUIDE_URL = "https://github.com/Bria-AI/obs-backgroundremoval/tree/support_analytics_and_update_readme?tab=readme-ov-file";

extern "C" void bria_show_welcome_dialog(void)
{
	QMainWindow *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());

	QDialog dialog(main);
	dialog.setWindowTitle("Bria - Remove Background");
	dialog.setFixedWidth(480);
	dialog.setModal(true);

	QVBoxLayout *root = new QVBoxLayout(&dialog);
	root->setSpacing(0);
	root->setContentsMargins(32, 32, 32, 28);

	// ── Headline ────────────────────────────────────────────────────────────
	QLabel *headline = new QLabel("V-RMBG 3.0 is installed!");
	{
		QFont f = headline->font();
		f.setPointSize(16);
		f.setBold(true);
		headline->setFont(f);
	}
	root->addWidget(headline);
	root->addSpacing(8);

	// ── Subheadline ──────────────────────────────────────────────────────────
	QLabel *sub = new QLabel("Remove your background in seconds.");
	{
		QFont f = sub->font();
		f.setPointSize(11);
		sub->setFont(f);
	}
	root->addWidget(sub);
	root->addSpacing(20);

	// ── Divider ──────────────────────────────────────────────────────────────
	QFrame *divider = new QFrame();
	divider->setFrameShape(QFrame::HLine);
	divider->setFrameShadow(QFrame::Sunken);
	root->addWidget(divider);
	root->addSpacing(16);

	// ── Quick Start title ────────────────────────────────────────────────────
	QLabel *qsTitle = new QLabel("Quick Start");
	{
		QFont f = qsTitle->font();
		f.setPointSize(10);
		f.setBold(true);
		qsTitle->setFont(f);
	}
	root->addWidget(qsTitle);
	root->addSpacing(10);

	// ── Steps ────────────────────────────────────────────────────────────────
	struct Step {
		const char *number;
		const char *text;
	};
	const Step steps[] = {
		{"1", "Right-click your camera source in OBS"},
		{"2", "Click <b>Filters</b>"},
		{"3", "Click <b>+</b> and add <b>'Bria \u2013 Remove Background'</b>"},
	};

	for (const auto &s : steps) {
		QHBoxLayout *row = new QHBoxLayout();
		row->setSpacing(10);
		row->setContentsMargins(4, 0, 0, 0);

		QLabel *num = new QLabel(s.number);
		{
			QFont f = num->font();
			f.setPointSize(10);
			f.setBold(true);
			num->setFont(f);
			num->setFixedWidth(16);
			num->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
		}

		QLabel *txt = new QLabel(s.text);
		txt->setTextFormat(Qt::RichText);
		{
			QFont f = txt->font();
			f.setPointSize(10);
			txt->setFont(f);
		}
		txt->setWordWrap(true);

		row->addWidget(num);
		row->addWidget(txt, 1);
		root->addLayout(row);
		root->addSpacing(6);
	}

	root->addSpacing(20);

	// ── Buttons ───────────────────────────────────────────────────────────────
	QHBoxLayout *btnRow = new QHBoxLayout();
	btnRow->setSpacing(12);
	btnRow->addStretch();

	QPushButton *btnGuide = new QPushButton("View Setup Guide");
	btnGuide->setFixedHeight(34);
	btnGuide->setMinimumWidth(140);

	QPushButton *btnStart = new QPushButton("Get Started");
	btnStart->setDefault(true);
	btnStart->setFixedHeight(34);
	btnStart->setMinimumWidth(120);

	btnRow->addWidget(btnGuide);
	btnRow->addWidget(btnStart);
	root->addLayout(btnRow);

	QObject::connect(btnStart, &QPushButton::clicked, [&dialog]() {
		BriaAnalytics::instance().capture("obs_plugin_get_started_clicked",
						  {{"source", "welcome_dialog"}});
		dialog.accept();
	});
	QObject::connect(btnGuide, &QPushButton::clicked, [&dialog]() {
		BriaAnalytics::instance().capture("obs_plugin_view_setup_guide_clicked",
						  {{"source", "welcome_dialog"}});
		QDesktopServices::openUrl(QUrl(SETUP_GUIDE_URL));
		dialog.accept();
	});

	dialog.exec();

	if (dialog.result() == QDialog::Rejected)
		BriaAnalytics::instance().capture("obs_plugin_welcome_dialog_dismissed",
						  {{"source", "welcome_dialog"}});
}
