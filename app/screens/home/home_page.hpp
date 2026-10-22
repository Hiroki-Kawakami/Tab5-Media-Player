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
    virtual bool shows_back() const { return true; }
    virtual bool can_go_back() const { return false; }
    virtual void go_back() {}
    virtual void go_to_root() {}
    virtual void build_navigation(lv_obj_t *) {}
    virtual void build(lv_obj_t *contents) = 0;
    virtual void save_state() {}
    virtual void on_appear() {}
    virtual void on_disappear() {}
    virtual bool is_under(const std::string &) const { return false; }

protected:
    friend class HomeScreen;
    HomeScreen *home_ = nullptr;
};
