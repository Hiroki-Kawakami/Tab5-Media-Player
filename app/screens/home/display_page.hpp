/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <string>
#include "home_page.hpp"

class DisplayPage : public HomePage {
public:
    const std::string &title() const override { return title_; }
    void build(lv_obj_t *contents) override;

private:
    const std::string title_ = "Display";
};
