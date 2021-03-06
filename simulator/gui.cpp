#include "gui.h"
#include "config/scalar.h"
#include "motor.h"
#include "util/clarke_transform.h"
#include "util/conversions.h"
#include "util/math_constants.h"
#include "util/rotation.h"
#include "util/sine_series.h"
#include <absl/strings/str_format.h>
#include <imgui.h>
#include <implot.h>
#include <random>

constexpr int kPlotHeight = 250; // sec
constexpr int kPlotWidth = -1;   // sec
const char* kAdvancedMotorChars = "Advanced Motor Config";

struct RollingPlotParams {
    int count;
    int begin;
    Scalar begin_time;
    Scalar end_time;
};

// This auto scroll implementation is janky
// it breaks the zooming functionality so it only activates half the time
// (see comment on can_trigger_auto_scroll)
struct AutoScroller {
    float last_y_min = -10;
    float last_y_max = 10;
    float last_y_range = last_y_max - last_y_min;
    bool can_trigger_auto_scroll = false;
};

// call before BeginPlot
void implot_autoscroll_next_plot(Scalar latest_data, AutoScroller* ctx) {
    // auto scroll
    if (ctx->can_trigger_auto_scroll) {
        // if the range is tiny and the curve is wobbling, auto scroll will go
        // crazy - constantly hitting max and min bounds
        Scalar range_adjust = 0;
        if (ctx->last_y_range < 1e-5) {
            range_adjust = 1e-5;
        }

        if (latest_data < ctx->last_y_min) {
            ImPlot::SetNextPlotLimitsY(
                latest_data, latest_data + ctx->last_y_range + range_adjust,
                ImGuiCond_Always);
        } else if (latest_data > ctx->last_y_max) {
            ImPlot::SetNextPlotLimitsY(latest_data - ctx->last_y_range -
                                           range_adjust,
                                       latest_data, ImGuiCond_Always);
        }
    }
    ctx->can_trigger_auto_scroll = !ctx->can_trigger_auto_scroll;
}

// call after BeginPlot, before EndPlot
void implot_update_autoscroll(AutoScroller* ctx) {
    const auto plot_limits = ImPlot::GetPlotLimits(0);
    ctx->last_y_min = plot_limits.Y.Min;
    ctx->last_y_max = plot_limits.Y.Max;
    ctx->last_y_range = ctx->last_y_max - ctx->last_y_min;
}

RollingPlotParams get_rolling_plot_params(const RollingBuffers& buffers,
                                          const Scalar rolling_history) {

    RollingPlotParams params;

    params.count = get_rolling_buffer_count(buffers.ctx);
    params.begin = get_rolling_buffer_begin(buffers.ctx);
    if (params.count != 0) {
        params.begin_time = buffers.timestamps[params.begin];
        params.end_time =
            buffers.timestamps[get_rolling_buffer_back(buffers.ctx)];
    } else {
        // no data to display
        params.begin_time = 0;
        params.end_time = 0;
    }

    params.begin_time =
        std::max(params.begin_time, params.end_time - rolling_history);

    return params;
}

void init_viz_data(VizData* viz_data) {
    const int num_pts = viz_data->circle_xs.size();
    for (int i = 0; i < num_pts; ++i) {
        viz_data->circle_xs[i] = std::cos(Scalar(i) / (num_pts - 1) * 2 * kPI);
        viz_data->circle_ys[i] = std::sin(Scalar(i) / (num_pts - 1) * 2 * kPI);
    }
}

uint32_t get_coil_color(int coil, float alpha) {
    switch (coil) {
    case 0:
        return ImColor(0.0f, 0.7490196228f, 1.0f, alpha); // DeepSkyBlue
    case 1:
        return ImColor(1.0f, 0.0f, 0.0f, alpha); // Red
    case 2:
        return ImColor(0.4980392158f, 1.0f, 0.0f, alpha); // Greens
    default:
        printf("Unhandled coil color %d\n", coil);
        exit(-1);
        return -1;
    }
}

void draw_electrical_plot(const RollingPlotParams& params,
                          const RollingBuffers& buffers, VizOptions* options) {
    if (ImGui::CollapsingHeader("Coil Visibility")) {
        ImGui::Checkbox("0", &options->coil_visible[0]);
        ImGui::SameLine();
        ImGui::Checkbox("1", &options->coil_visible[1]);
        ImGui::SameLine();
        ImGui::Checkbox("2", &options->coil_visible[2]);
    }

    ImPlot::SetNextPlotLimitsX(params.begin_time, params.end_time,
                               ImGuiCond_Always);
    ImPlot::SetNextPlotLimitsY(-10, 10, ImGuiCond_Once);
    if (ImPlot::BeginPlot("Phase Currents", "Seconds", "Amperes",
                          ImVec2(kPlotWidth, kPlotHeight))) {
        for (int i = 0; i < 3; ++i) {
            if (!options->coil_visible[i]) {
                continue;
            }
            ImPlot::PushStyleColor(ImPlotCol_Line, get_coil_color(i, 1.0f));
            ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 1.0f);
            ImPlot::PlotLine(absl::StrFormat("Coil %d", i).c_str(),
                             buffers.timestamps.data(),
                             buffers.phase_currents[i].data(), params.count,
                             params.begin, sizeof(Scalar));
            ImPlot::PopStyleVar();
            ImPlot::PopStyleColor();
        }

        ImPlot::EndPlot();
    }
}

void draw_torque_plot(const RollingPlotParams& params,
                      const RollingBuffers& buffers) {
    ImPlot::SetNextPlotLimitsX(params.begin_time, params.end_time,
                               ImGuiCond_Always);
    ImPlot::SetNextPlotLimitsY(-2, 2, ImGuiCond_Once);

    static AutoScroller as;
    if (get_rolling_buffer_count(buffers.ctx) > 0) {
        const int last_idx = get_rolling_buffer_back(buffers.ctx);
        const Scalar last_torque = buffers.torque[last_idx];
        implot_autoscroll_next_plot(last_torque, &as);
    }

    if (ImPlot::BeginPlot("Torque", "Seconds", "N . m",
                          ImVec2(kPlotWidth, kPlotHeight))) {
        ImPlot::PlotLine("", buffers.timestamps.data(), buffers.torque.data(),
                         params.count, params.begin, sizeof(Scalar));

        implot_update_autoscroll(&as);
        ImPlot::EndPlot();
    }
}

void draw_power_plot(const RollingPlotParams& params,
                     const RollingBuffers& buffers) {
    ImPlot::SetNextPlotLimitsX(params.begin_time, params.end_time,
                               ImGuiCond_Always);
    ImPlot::SetNextPlotLimitsY(-2, 2, ImGuiCond_Once);

    static AutoScroller as;
    if (get_rolling_buffer_count(buffers.ctx) > 0) {
        const int last_idx = get_rolling_buffer_back(buffers.ctx);
        const Scalar last_torque = buffers.power_draw[last_idx];
        implot_autoscroll_next_plot(last_torque, &as);
    }

    if (ImPlot::BeginPlot("Power Draw", "Seconds", "Watts",
                          ImVec2(kPlotWidth, kPlotHeight))) {
        ImPlot::PlotLine("", buffers.timestamps.data(),
                         buffers.power_draw.data(), params.count, params.begin,
                         sizeof(Scalar));

        implot_update_autoscroll(&as);
        ImPlot::EndPlot();
    }
}

void draw_rotor_angular_vel_plot(const RollingPlotParams& params,
                                 const RollingBuffers& buffers) {
    ImPlot::SetNextPlotLimitsX(params.begin_time, params.end_time,
                               ImGuiCond_Always);

    ImPlot::SetNextPlotLimitsY(-10, 10, ImGuiCond_Once);

    static AutoScroller as;
    if (get_rolling_buffer_count(buffers.ctx) > 0) {
        const int last_idx = get_rolling_buffer_back(buffers.ctx);
        const Scalar last_angular_vel = buffers.rotor_angular_vel[last_idx];
        implot_autoscroll_next_plot(last_angular_vel, &as);
    }

    if (ImPlot::BeginPlot("Rotor Angular Vel", "Seconds", "Radians / Sec",
                          ImVec2(kPlotWidth, kPlotHeight))) {

        ImPlot::PlotLine("", buffers.timestamps.data(),
                         buffers.rotor_angular_vel.data(), params.count,
                         params.begin, sizeof(Scalar));

        implot_update_autoscroll(&as);

        ImPlot::EndPlot();
    }
}

void update_rolling_buffers(const Scalar time, const BoardState& board,
                            const MotorState& motor, const FocState& foc,
                            RollingBuffers* buffers) {
    const int next_idx = buffers->ctx.next_idx;

    buffers->timestamps[next_idx] = time;

    const Eigen::Matrix<Scalar, 3, 1> pole_voltages = get_pole_voltages(
        board.bus_voltage, motor.electrical.phase_currents, board.gate);

    const Eigen::Matrix<Scalar, 3, 1> phase_voltages =
        get_phase_voltages(pole_voltages, motor.electrical.bEmfs);

    for (int i = 0; i < 3; ++i) {
        buffers->phase_vs[i][next_idx] = phase_voltages(i);

        buffers->phase_currents[i][next_idx] =
            motor.electrical.phase_currents(i);

        buffers->bEmfs[i][next_idx] = motor.electrical.bEmfs(i);

        buffers->normed_bEmfs[i][next_idx] = motor.electrical.normed_bEmfs(i);

        buffers->pwm_duties[i][next_idx] = board.pwm.duties[i];

        buffers->pwm_level[next_idx] = board.pwm.level;

        Scalar gate_state = board.gate.actual[i];
        if (gate_state == OFF) {
            // map the indeterminate state to -0.5
            gate_state = -0.5;
        }
        buffers->gate_states[i][next_idx] = gate_state;

        // Project current onto qd axes
        {
            const Scalar q_axis_electrical_angle = get_q_axis_electrical_angle(
                motor.params.num_pole_pairs, motor.kinematic.rotor_angle);
            const std::complex<Scalar> park_transform =
                get_rotation(-q_axis_electrical_angle);
            const std::complex<Scalar> current_qd =
                park_transform *
                clarke_transform(motor.electrical.phase_currents);
            buffers->current_q[next_idx] = current_qd.real();
            buffers->current_d[next_idx] = current_qd.imag();
        }

        buffers->current_q_err[next_idx] = foc.iq_controller.err;

        buffers->current_q_integral[next_idx] = foc.iq_controller.integral;

        buffers->current_d_err[next_idx] = foc.id_controller.err;

        buffers->current_d_integral[next_idx] = foc.id_controller.integral;
    }

    // power calculation
    {
        // power is v*i for all i's that are flowing into the gates
        Scalar power_draw = 0;
        for (int i = 0; i < 3; ++i) {
            if (board.gate.actual[i] == HIGH) {
                power_draw +=
                    board.bus_voltage * motor.electrical.phase_currents(i);
            }
        }
        buffers->power_draw[next_idx] = power_draw;
    }

    buffers->current_d_integral[next_idx] = foc.id_controller.integral;

    buffers->rotor_angular_vel[next_idx] = motor.kinematic.rotor_angular_vel;
    buffers->torque[next_idx] = motor.kinematic.torque;

    rolling_buffer_advance_idx(&buffers->ctx);
}

void draw_pwm_plot(const RollingPlotParams& params,
                   const RollingBuffers& buffers) {
    ImPlot::SetNextPlotLimitsX(params.begin_time, params.end_time,
                               ImGuiCond_Always);
    ImPlot::SetNextPlotLimitsY(-0.1, 1.1, ImGuiCond_Once);
    if (ImPlot::BeginPlot("PWM", "Seconds", "",
                          ImVec2(kPlotWidth, kPlotHeight))) {
        for (int i = 0; i < 3; ++i) {
            ImPlot::PlotLine(absl::StrFormat("Gate %d", i).c_str(),
                             buffers.timestamps.data(),
                             buffers.pwm_duties[i].data(), params.count,
                             params.begin, sizeof(Scalar));
        }
        ImPlot::PushStyleColor(ImPlotCol_Line,
                               (uint32_t)ImColor(1.0f, 1.0f, 1.0f, 0.2));
        ImPlot::PlotLine("Level", buffers.timestamps.data(),
                         buffers.pwm_level.data(), params.count, params.begin,
                         sizeof(Scalar));
        ImPlot::PopStyleColor();

        ImPlot::EndPlot();
    }
}

void draw_gate_plot(const RollingPlotParams& params,
                    const RollingBuffers& buffers) {

    ImPlot::SetNextPlotLimitsX(params.begin_time, params.end_time,
                               ImGuiCond_Always);
    ImPlot::SetNextPlotLimitsY(-0.6, 1.1, ImGuiCond_Always);

    // this mapping is established in update_rolling_buffers
    static double yticks[] = {-0.5, 0, 1};
    static const char* ylabels[] = {"OFF", "LOW", "HIGH"};
    ImPlot::SetNextPlotTicksY(yticks, 3, ylabels);

    if (ImPlot::BeginPlot("Gate States", "Seconds", "",
                          ImVec2(kPlotWidth, kPlotHeight))) {
        for (int i = 0; i < 3; ++i) {
            ImPlot::PlotLine(absl::StrFormat("Gate %d", i).c_str(),
                             buffers.timestamps.data(),
                             buffers.gate_states[i].data(), params.count,
                             params.begin, sizeof(Scalar));
        }
        ImPlot::EndPlot();
    }
}

void draw_current_qd_plot(const RollingPlotParams& params,
                          const RollingBuffers& buffers) {
    ImPlot::SetNextPlotLimitsX(params.begin_time, params.end_time,
                               ImGuiCond_Always);

    if (ImPlot::BeginPlot("Current qd", "Seconds", nullptr,
                          ImVec2(kPlotWidth, kPlotHeight))) {
        ImPlot::PlotLine("iq", buffers.timestamps.data(),
                         buffers.current_q.data(), params.count, params.begin,
                         sizeof(Scalar));

        ImPlot::PlotLine("id", buffers.timestamps.data(),
                         buffers.current_d.data(), params.count, params.begin,
                         sizeof(Scalar));
        ImPlot::EndPlot();
    }
}
void draw_current_qd_err_plot(const RollingPlotParams& params,
                              const RollingBuffers& buffers) {

    ImPlot::SetNextPlotLimitsX(params.begin_time, params.end_time,
                               ImGuiCond_Always);
    ImPlot::SetNextPlotLimitsY(-1, 1, ImGuiCond_Once);
    if (ImPlot::BeginPlot("Current Controller Errors", "Seconds", nullptr,
                          ImVec2(kPlotWidth, kPlotHeight))) {
        ImPlot::PlotLine("iq error", buffers.timestamps.data(),
                         buffers.current_q_err.data(), params.count,
                         params.begin, sizeof(Scalar));

        ImPlot::PlotLine("id error", buffers.timestamps.data(),
                         buffers.current_d_err.data(), params.count,
                         params.begin, sizeof(Scalar));
        ImPlot::EndPlot();
    }
}

void draw_current_qd_integral_plot(const RollingPlotParams& params,
                                   const RollingBuffers& buffers) {

    ImPlot::SetNextPlotLimitsX(params.begin_time, params.end_time,
                               ImGuiCond_Always);

    if (ImPlot::BeginPlot("Current Controller Integrals", "Seconds", nullptr,
                          ImVec2(kPlotWidth, kPlotHeight))) {
        ImPlot::PlotLine("iq int", buffers.timestamps.data(),
                         buffers.current_q_integral.data(), params.count,
                         params.begin, sizeof(Scalar));

        ImPlot::PlotLine("id int", buffers.timestamps.data(),
                         buffers.current_d_integral.data(), params.count,
                         params.begin, sizeof(Scalar));
        ImPlot::EndPlot();
    }
}

void implot_radial_line(const char* name, float inner_radius,
                        float outer_radius, float angle) {
    const float cos_angle = (float)std::cos(angle);
    const float sin_angle = (float)std::sin(angle);
    std::array<float, 2> xs = {inner_radius * cos_angle,
                               outer_radius * cos_angle};
    std::array<float, 2> ys = {inner_radius * sin_angle,
                               outer_radius * sin_angle};
    ImPlot::PlotLine(name, xs.data(), ys.data(), 2);
}

void implot_central_line(const char* name, float x, float y) {
    std::array<float, 2> xs = {0, x};
    std::array<float, 2> ys = {0, y};
    ImPlot::PlotLine(name, xs.data(), ys.data(), 2);
}

void draw_rotor_plot(const VizData& viz_data, const Scalar rotor_angle) {
    ImPlot::SetNextPlotLimits(/*x_min=*/-1.0,
                              /*x_max=*/1.0,
                              /*y_min=*/-1.0,
                              /*y_max=*/1.0);
    if (ImPlot::BeginPlot("##Rotor Angle", nullptr, nullptr, ImVec2{75, 75},
                          ImPlotFlags_Default & !ImPlotFlags_Legend,
                          ImPlotAxisFlags_Default & ~ImPlotAxisFlags_TickLabels,
                          ImPlotAxisFlags_Default &
                              ~ImPlotAxisFlags_TickLabels)) {

        ImPlot::PushStyleColor(ImPlotCol_Line,
                               (uint32_t)ImColor(1.0f, 1.0f, 1.0f, 1.0));
        implot_radial_line("Rotor Angle", 0.0f, 1.0f, rotor_angle);

        ImPlot::PushStyleColor(ImPlotCol_Line,
                               (uint32_t)ImColor(1.0f, 1.0f, 1.0f, 0.2));

        ImPlot::PlotLine("Rotor Circle", viz_data.circle_xs.data(),
                         viz_data.circle_ys.data(), viz_data.circle_xs.size());

        ImPlot::PopStyleColor(2);

        ImPlot::EndPlot();
    }
}

void draw_space_vector_plot(const SimState& state, VizOptions* options) {
    ImGui::Checkbox("Use Rotor Frame", &options->use_rotor_frame);

    static float log_limits = -10;
    ImGui::SliderFloat("Log Limits", &log_limits, -10, 3);

    const float limits = std::exp(log_limits);

    ImPlot::SetNextPlotLimits(/*x_min=*/-limits,
                              /*x_max=*/limits,
                              /*y_min=*/-limits,
                              /*y_max=*/limits, ImGuiCond_Always);
    if (ImPlot::BeginPlot("##Space Vector Plot", nullptr, nullptr,
                          ImVec2{300, 300}, ImPlotFlags_Default,
                          ImPlotAxisFlags_Default & ~ImPlotAxisFlags_TickLabels,
                          ImPlotAxisFlags_Default &
                              ~ImPlotAxisFlags_TickLabels)) {

        const Scalar electrical_angle =
            get_electrical_angle(state.motor.params.num_pole_pairs,
                                 state.motor.kinematic.rotor_angle);
        ImPlot::PushStyleColor(ImPlotCol_Line,
                               (uint32_t)ImColor(1.0f, 1.0f, 1.0f, 1.0));
        implot_radial_line("Rotor Angle", 0.0f, 1.0f,
                           options->use_rotor_frame ? 0 : electrical_angle);
        ImPlot::PopStyleColor();

        const std::complex<Scalar> park_transform =
            get_rotation(-electrical_angle);

        const Eigen::Matrix<Scalar, 3, 1> pole_voltages = get_pole_voltages(
            state.board.bus_voltage, state.motor.electrical.phase_currents,
            state.board.gate);

        std::complex<Scalar> pole_voltage_sv = clarke_transform(pole_voltages);

        if (options->use_rotor_frame) {
            pole_voltage_sv *= park_transform;
        }

        ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 3.0f);
        implot_central_line("Pole Voltage", pole_voltage_sv.real(),
                            pole_voltage_sv.imag());
        ImPlot::PopStyleVar();

        std::complex<Scalar> current_sv =
            clarke_transform(state.motor.electrical.phase_currents);

        if (options->use_rotor_frame) {
            current_sv *= park_transform;
        }

        implot_central_line("Current", current_sv.real(), current_sv.imag());

        std::complex<Scalar> normed_bEmf_sv =
            clarke_transform(state.motor.electrical.normed_bEmfs);

        if (options->use_rotor_frame) {
            normed_bEmf_sv *= park_transform;
        }

        implot_central_line("Normed bEmf ", normed_bEmf_sv.real(),
                            normed_bEmf_sv.imag());

        if (state.commutation_mode == kCommutationModeFOC) {
            auto voltage_sv = state.foc.voltage_qd;
            const std::complex<Scalar> rot90{0, -1};
            voltage_sv *= rot90; // put into rotor frame

            if (!options->use_rotor_frame) {
                voltage_sv *= std::conj(park_transform);
            }

            implot_central_line("FOC Voltage Desired", voltage_sv.real(),
                                voltage_sv.imag());
        }

        std::complex<Scalar> bEmf_sv =
            clarke_transform(state.motor.electrical.bEmfs);

        if (options->use_rotor_frame) {
            bEmf_sv *= park_transform;
        }

        implot_central_line("bEmf", bEmf_sv.real(), bEmf_sv.imag());

        ImPlot::EndPlot();
    }
}

bool order_of_magnitude_control(const char* label, Scalar* controllee,
                                const int exp_min = -4, const int exp_max = 4) {
    bool interacted = false;
    Scalar l10 = std::log10(*controllee);

    int integral_part = int(l10);
    float fractional_part = float(l10 - integral_part);

    while (fractional_part < 0) {
        --integral_part;
        ++fractional_part;
    }

    // 10^(integral_part + fractional_part) =
    // 10^integral_part * base
    float base = std::pow(10, fractional_part);

    ImGui::Text(label);
    ImGui::SameLine();
    ImGui::Text("%fe%d", base, integral_part);
    ImGui::PushID(label);
    interacted = ImGui::SliderFloat("mantissa", &base, 1.0f, 9.99f);
    interacted = ImGui::SliderInt("exponent (base 10)", &integral_part, exp_min,
                                  exp_max) ||
                 interacted;
    ImGui::PopID();

    *controllee = base * std::pow(10, integral_part);

    return interacted;
}

// imgui doesn't have SliderDouble
bool Slider(const char* label, Scalar* scalar, Scalar low, Scalar high) {
    float wrapped = *scalar;
    const bool interacted =
        ImGui::SliderFloat(label, &wrapped, float(low), float(high));
    *scalar = Scalar(wrapped);
    return interacted;
}

bool ScaledSlider(const Scalar scale, const char* label, Scalar* scalar,
                  Scalar low, Scalar high) {
    float wrapped = *scalar * scale;
    const bool interacted =
        ImGui::SliderFloat(label, &wrapped, float(low), float(high));
    *scalar = Scalar(wrapped) / scale;
    return interacted;
}

void run_advanced_motor_config(MotorState* motor_ptr) {
    MotorState& motor = *motor_ptr; // convenience ref

    if (ImGui::BeginTabBar("##Advanced Motor Control Options")) {
        if (ImGui::BeginTabItem("Back EMF Curve")) {
            ImGui::Text(
                "normed_bEmf(e) =  overal_scale * (a1 sin(e) + a3 sin(3e) "
                "+ a5 sin(5e) + a7 "
                "sin(7e) + a9 sin(9e))");

            const auto to_gui_scale =
                [](const Eigen::Matrix<Scalar, 5, 1>& in) {
                    Eigen::Matrix<Scalar, 5, 1> out = in;
                    for (int i = 1; i < out.size(); ++i) {
                        out(i) /= in(0);
                    }
                    return out;
                };

            const auto from_gui_scale =
                [](const Eigen::Matrix<Scalar, 5, 1>& in) {
                    Eigen::Matrix<Scalar, 5, 1> out = in;
                    for (int i = 1; i < out.size(); ++i) {
                        out(i) *= in(0);
                    }
                    return out;
                };

            Eigen::Matrix<Scalar, 5, 1> gui_scale =
                to_gui_scale(motor.params.normed_bEmf_coeffs);

            ImGui::Text("Presets");
            ImGui::SameLine();
            if (ImGui::Button("Sine Wave")) {
                gui_scale.tail<4>().setZero();
            }
            ImGui::SameLine();
            if (ImGui::Button("Trapezoid")) {
                gui_scale(1) = 0.278;
                gui_scale(2) = 0.119;
                gui_scale(3) = 0.053;
                gui_scale(4) = 0.029;
            }

            ScaledSlider(1000, "overall_scale * 1000", &gui_scale(0), 1, 500);

            // additional harmonics
            for (int i = 1; i < 5; ++i) {
                Slider(absl::StrFormat("a%d", 2 * i + 1).c_str(), &gui_scale(i),
                       0, 1);
            }

            motor.params.normed_bEmf_coeffs = from_gui_scale(gui_scale);

            constexpr int kNumSamples = 1000;
            static std::array<Scalar, kNumSamples> angles;
            static std::array<Scalar, kNumSamples> samples;
            for (int i = 0; i < kNumSamples; ++i) {
                const Scalar angle = 2 * kPI * Scalar(i) / kNumSamples;
                Eigen::Matrix<Scalar, 5, 1> odd_sine_series;
                generate_odd_sine_series(5, angle, odd_sine_series.data());
                samples[i] =
                    odd_sine_series.dot(motor.params.normed_bEmf_coeffs);
                angles[i] = angle;
            }

            ImPlot::SetNextPlotLimitsX(0, 2 * kPI, ImGuiCond_Once);
            ImPlot::SetNextPlotLimitsY(
                -1.5 * motor.params.normed_bEmf_coeffs(0),
                1.5 * motor.params.normed_bEmf_coeffs(0), ImGuiCond_Once);

            if (ImPlot::BeginPlot("Normed Back Emf", "Electrical Angle (rad)",
                                  "Volt . sec",
                                  ImVec2(kPlotWidth, kPlotHeight))) {
                ImPlot::PlotLine("", angles.data(), samples.data(),
                                 angles.size());
                ImPlot::EndPlot();
            }

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Cogging Torque")) {

            // convenience reference
            auto& cogging_torque_map = motor.params.cogging_torque_map;

            if (ImGui::Button("Set Cogging Torque to Zero")) {
                cogging_torque_map = {};
            }

            if (ImGui::Button("Generate Random Cogging Torque Map")) {
                std::random_device rd{};
                std::mt19937 gen{rd()};
                std::normal_distribution<> d{0, 1};

                // cos terms are even idx
                // sin terms are odd idx
                std::array<Scalar, 12> fourier_coeffs;
                // arbitrarily choose some frequencies that might be dominant in
                // the cogging map, plus some fuding to make it look interesting
                const int p = motor.params.num_pole_pairs;
                std::array<int, 6> fourier_frequencies{
                    1, p, p * 2 + 1, p * 3 + 2, p * 7 + 3, p * 10 + 4};

                std::array<Scalar, 6> fourier_frequencies_scale{0.5, 1.5, 1.0,
                                                                1.5, 0.5, 0.25};

                for (int i = 0; i < 12; ++i) {
                    fourier_coeffs[i] =
                        d(gen) * fourier_frequencies_scale[i / 2];
                }

                for (int i = 0; i < cogging_torque_map.size(); ++i) {
                    const Scalar progress =
                        Scalar(i) / cogging_torque_map.size();
                    Scalar val = 0;
                    for (int n = 0; n < 6; ++n) {
                        const Scalar k1 = fourier_coeffs[2 * n];
                        const Scalar k2 = fourier_coeffs[2 * n + 1];
                        const Scalar c = std::cos(progress * 2 * kPI *
                                                  fourier_frequencies[n]);
                        const Scalar s = std::sin(progress * 2 * kPI *
                                                  fourier_frequencies[n]);
                        val += k1 * c + k2 * s;
                    }
                    cogging_torque_map[i] = val;
                }

                // // the first entry must equal the last entry
                // // so de-slope everything
                // const Scalar slope =
                //     (cogging_torque_map.back() - cogging_torque_map[0]) /
                //     cogging_torque_map.size();
                // for (int i = 0; i < cogging_torque_map.size(); ++i) {
                //     cogging_torque_map[i] -= slope * i;
                // }

                // // the integral of all cogging torque needs to be zero for
                // energy
                // // conservation, and then scaled to a realistic max torque
                // value Scalar avg = 0; for (int i = 0; i <
                // cogging_torque_map.size(); ++i) {
                //     avg += cogging_torque_map[i];
                // }
                // avg /= cogging_torque_map.size();

                // // recenter
                // for (int i = 0; i < cogging_torque_map.size(); ++i) {
                //     cogging_torque_map[i] -= avg;
                // }

                // rescale
                Scalar max_abs = 0;
                for (const Scalar torque : cogging_torque_map) {
                    max_abs = std::max(max_abs, std::abs(torque));
                }

                for (Scalar& torque : cogging_torque_map) {
                    torque *= 0.01 / max_abs;
                }

                // sanity check energy conservation
                Scalar energy = 0;
                for (const Scalar torque : cogging_torque_map) {
                    energy += torque;
                }
                energy *= 2 * kPI / cogging_torque_map.size();
                if (std::abs(energy) > 1e-8) {
                    printf("Energy conservation violated by cogging map\n");
                }
            }

            ImPlot::SetNextPlotLimitsX(0, cogging_torque_map.size(),
                                       ImGuiCond_Once);

            ImPlot::SetNextPlotLimitsY(-0.01, 0.01, ImGuiCond_Once);

            if (ImPlot::BeginPlot("Cogging Torque", "encoder idx", "N . m",
                                  ImVec2(kPlotWidth, kPlotHeight))) {
                ImPlot::PlotLine("", cogging_torque_map.data(),
                                 cogging_torque_map.size());
                ImPlot::EndPlot();
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

void run_gui(const VizData& viz_data, VizOptions* options,
             SimState* sim_state) {

    ImGui::Begin("Simulation Control");
    ImGui::Columns(2);
    ImGui::SetColumnWidth(0, 120);

    draw_rotor_plot(viz_data, sim_state->motor.kinematic.rotor_angle);

    ImGui::NextColumn();

    ImGui::Text("Simulation Time: %f", sim_state->time);
    if (sim_state->paused) {
        sim_state->paused = !ImGui::Button("Resume");
    } else {
        sim_state->paused = ImGui::Button("Pause");
    }
    ImGui::SliderInt("Step Multiplier", &sim_state->step_multiplier, 1, 5000);
    ImGui::SliderFloat("Rolling History (sec)", &options->rolling_history,
                       0.001f, 1.0f);

    ImGui::Columns(1);

    ImGui::Text("Rotor Angle %f", sim_state->motor.kinematic.rotor_angle);

    ImGui::NewLine();
    ImGui::Text("Space Vectors");
    draw_space_vector_plot(*sim_state, options);

    if (ImGui::BeginTabBar("##Options")) {
        if (ImGui::BeginTabItem("Commutation Control")) {
            ImGui::RadioButton("Manual", &sim_state->commutation_mode,
                               kCommutationModeManual);
            ImGui::SameLine();
            ImGui::RadioButton("Six Step", &sim_state->commutation_mode,
                               kCommutationModeSixStep);
            ImGui::SameLine();
            ImGui::RadioButton("FOC", &sim_state->commutation_mode,
                               kCommutationModeFOC);

            ImGui::NewLine();
            if (sim_state->commutation_mode == kCommutationModeManual) {
                // manual controls
                ImGui::Text("Manual Command");
                for (int i = 0; i < 3; ++i) {
                    ImGui::Text("Gate %d", i);
                    ImGui::SameLine();
                    ImGui::PushID(i);

                    int current_command =
                        (int)sim_state->board.gate.commanded[i];
                    ImGui::RadioButton(absl::StrFormat("HIGH", i).c_str(),
                                       &current_command, 1);
                    ImGui::SameLine();
                    ImGui::RadioButton(absl::StrFormat("LOW", i).c_str(),
                                       &current_command, 0);

                    sim_state->board.gate.commanded[i] = (bool)current_command;
                    ImGui::PopID();
                }
            }

            if (sim_state->commutation_mode == kCommutationModeSixStep) {
                Slider("Phase Advance", &sim_state->six_step_phase_advance,
                       -0.5, 0.5);
            }

            if (sim_state->commutation_mode == kCommutationModeFOC) {
                order_of_magnitude_control("Update Period (sec)",
                                           &sim_state->foc.period, -5, -2);

                const Scalar update_freq = 1.0 / sim_state->foc.period;
                if (update_freq < 1000) {
                    // decide whether to display Hz vs kHz
                    // todo: make this prettier
                    ImGui::Text("=> Update Frequency %f Hz",
                                1.0 / sim_state->foc.period);
                } else {
                    ImGui::Text("=> Update Frequency %f kHz",
                                1.0 / sim_state->foc.period / 1000);
                }

                ImGui::NewLine();
                Slider("Load Torque", &sim_state->load_torque, -1.0, 1.0);

                static bool match_load_torque = false;

                if (!match_load_torque) {
                    Slider("Desired Torque", &sim_state->foc_desired_torque,
                           -1.0, 1.0);
                } else {
                    sim_state->foc_desired_torque = -sim_state->load_torque;
                }
                ImGui::Checkbox("Desired Torque = -Load Torque",
                                &match_load_torque);

                ImGui::NewLine();

                ImGui::Checkbox("Non-Sinusoidal Drive Mode",
                                &sim_state->foc_non_sinusoidal_drive_mode);
                ImGui::Checkbox("Cogging Compensation",
                                &sim_state->foc_use_cogging_compensation);
                ImGui::Checkbox("qd Decoupling",
                                &sim_state->foc_use_qd_decoupling);

                ImGui::NewLine();

                ImGui::Text("PI Params");
                static bool auto_pi_params = true;
                ImGui::SameLine();
                ImGui::Checkbox("Auto", &auto_pi_params);
                if (auto_pi_params) {
                    make_motor_pi_params(
                        /*bandwidth=*/10000,
                        /*resistance=*/sim_state->motor.params.phase_resistance,
                        /*inductance=*/
                        sim_state->motor.params.phase_inductance);
                    ImGui::Text("P Gain %f",
                                sim_state->foc.i_controller_params.p_gain);
                    ImGui::Text("I Gain %f",
                                sim_state->foc.i_controller_params.i_gain);
                } else {
                    ImGui::Checkbox("Anti-windup",
                                    &sim_state->foc_pi_anti_windup);
                    order_of_magnitude_control(
                        "P Gain", &sim_state->foc.i_controller_params.p_gain,
                        -1, 6);
                    order_of_magnitude_control(
                        "I Gain", &sim_state->foc.i_controller_params.i_gain,
                        -1, 6);
                }
            }

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("System Params")) {
            Slider("Load Torque", &sim_state->load_torque, -1.0, 1.0);

            ImGui::Text("Board Params");
            Slider("Bus Voltage", &sim_state->board.bus_voltage, 1.0, 120);
            Slider("Diode Active Voltage",
                   &sim_state->board.gate.diode_active_voltage, 0.0, 1.0);

            double dead_time_usec = sim_state->board.gate.dead_time * 1e6;
            if (Slider("Gate Dead Time (usec)", &dead_time_usec, 0.0f, 100)) {
                sim_state->board.gate.dead_time = dead_time_usec /= 1e6;
            }

            ImGui::Text("PWM Timer Resolution");
            static int pwm_resolution_bits = 0;
            ImGui::RadioButton("1 bit", &pwm_resolution_bits, 1);
            ImGui::SameLine();
            ImGui::RadioButton("8 bit", &pwm_resolution_bits, 8);
            ImGui::SameLine();
            ImGui::RadioButton("16 bit", &pwm_resolution_bits, 16);
            ImGui::SameLine();
            ImGui::RadioButton("Infinity", &pwm_resolution_bits, 0);
            if (pwm_resolution_bits == 0) {
                // infinity resolution
                sim_state->board.pwm.resolution = 0;
            } else {
                sim_state->board.pwm.resolution =
                    std::pow(2.0, -pwm_resolution_bits);
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Motor Params")) {
            ImGui::SliderInt("Num Pole Pairs",
                             &sim_state->motor.params.num_pole_pairs, 1, 8);
            Slider("Rotor Moment of Inertia (kg m^2)",
                   &sim_state->motor.params.rotor_inertia, 0.1, 10);
            order_of_magnitude_control(
                "Phase Inductance", &sim_state->motor.params.phase_inductance);
            order_of_magnitude_control(
                "Phase Resistance", &sim_state->motor.params.phase_resistance);

            if (ImGui::Button("Open Advanced Config")) {
                options->advanced_motor_config = true;
                ImGui::SetWindowFocus(kAdvancedMotorChars);
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();

    const RollingPlotParams rolling_plot_params = get_rolling_plot_params(
        viz_data.rolling_buffers, options->rolling_history);

    ImGui::Begin("Rolling Plots");
    if (ImGui::Button("Dump CSV to Clipboard")) {
        ImGui::LogToClipboard();
        ImGui::LogText(to_csv(viz_data.rolling_buffers).c_str());
        ImGui::LogFinish();
    }

    ImGui::Columns(3);
    draw_rotor_angular_vel_plot(rolling_plot_params, viz_data.rolling_buffers);
    ImGui::NextColumn();
    draw_torque_plot(rolling_plot_params, viz_data.rolling_buffers);
    ImGui::NextColumn();
    draw_power_plot(rolling_plot_params, viz_data.rolling_buffers);
    ImGui::Columns(1);

    if (sim_state->commutation_mode == kCommutationModeFOC) {
        ImGui::Columns(3);
        draw_pwm_plot(rolling_plot_params, viz_data.rolling_buffers);
        ImGui::NextColumn();
        draw_gate_plot(rolling_plot_params, viz_data.rolling_buffers);
        ImGui::NextColumn();
        draw_electrical_plot(rolling_plot_params, viz_data.rolling_buffers,
                             options);

    } else {
        ImGui::Columns(2);
        draw_gate_plot(rolling_plot_params, viz_data.rolling_buffers);
        ImGui::NextColumn();
        draw_electrical_plot(rolling_plot_params, viz_data.rolling_buffers,
                             options);
    }

    ImGui::Columns(1);

    if (sim_state->commutation_mode == kCommutationModeFOC) {
        ImGui::Columns(3);
        draw_current_qd_plot(rolling_plot_params, viz_data.rolling_buffers);
        ImGui::NextColumn();
        draw_current_qd_err_plot(rolling_plot_params, viz_data.rolling_buffers);
        ImGui::NextColumn();
        draw_current_qd_integral_plot(rolling_plot_params,
                                      viz_data.rolling_buffers);
    } else {
        draw_current_qd_plot(rolling_plot_params, viz_data.rolling_buffers);
    }

    ImGui::End();

    if (options->advanced_motor_config) {
        ImGui::Begin(kAdvancedMotorChars, &options->advanced_motor_config);
        run_advanced_motor_config(&sim_state->motor);
        ImGui::End();
    }
}

std::string to_csv(const RollingBuffers& rolling_buffers) {
    std::stringstream ss;

    using NamedField =
        std::pair<const char*, const std::array<Scalar, kNumRollingPts>*>;

    std::array<NamedField, 8> fields{
        std::make_pair("timestamp", &rolling_buffers.timestamps),
        std::make_pair("torque", &rolling_buffers.torque),
        std::make_pair("bEmf_a", &rolling_buffers.bEmfs[0]),
        std::make_pair("bEmf_b", &rolling_buffers.bEmfs[1]),
        std::make_pair("bEmf_c", &rolling_buffers.bEmfs[2]),
        std::make_pair("current_a", &rolling_buffers.phase_currents[0]),
        std::make_pair("current_b", &rolling_buffers.phase_currents[1]),
        std::make_pair("current_c", &rolling_buffers.phase_currents[2])};

    // write the headers
    for (int col = 0; col < fields.size(); ++col) {
        ss << fields[col].first;
        if (col + 1 != fields.size()) {
            // write the separator
            ss << ",";
        }
    }
    ss << "\n";

    // write the values
    const int num_rows = get_rolling_buffer_count(rolling_buffers.ctx);
    for (int row = 0; row < num_rows; ++row) {
        for (int col = 0; col < fields.size(); ++col) {
            // write the value
            ss << (*fields[col].second)[row];
            if (col + 1 != fields.size()) {
                // write the separator
                ss << ",";
            }
        }
        // row finished
        ss << "\n";
    }

    return ss.str();
}
