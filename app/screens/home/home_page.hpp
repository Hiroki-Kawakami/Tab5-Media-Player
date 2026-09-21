/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <string>
#include "lvgl.h"

class HomeScreen;

class HomePage {
public:
    virtual ~HomePage() = default;
    virtual const std::string &title() const = 0;
    virtual void build(lv_obj_t *contents) = 0;
    virtual void save_state() {}
    virtual void on_appear() {}
    virtual bool is_under(const std::string &) const { return false; }

protected:
    friend class HomeScreen;
    HomeScreen *home_ = nullptr;
};
