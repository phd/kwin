/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2007 Christian Nitschkowski <christian.nitschkowski@kdemail.net>
    SPDX-FileCopyrightText: 2023 Andrew Shark <ashark at linuxcomp.ru>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <kcmodule.h>

#include "ui_mousemark_config.h"

class KActionCollection;

namespace KWin
{

class MouseMarkEffectConfigForm : public QWidget, public Ui::MouseMarkEffectConfigForm
{
    Q_OBJECT
public:
    explicit MouseMarkEffectConfigForm(QWidget *parent);
};

class MouseMarkEffectConfig : public KCModule
{
    Q_OBJECT
public:
    explicit MouseMarkEffectConfig(QObject *parent, const KPluginMetaData &data);
    ~MouseMarkEffectConfig() override;

    void load() override;
    void save() override;

private:
    void updateSpinBoxSuffix();

    MouseMarkEffectConfigForm m_ui;
    KActionCollection *m_actionCollection;
};

} // namespace
