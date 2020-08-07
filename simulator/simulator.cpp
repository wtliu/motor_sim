#include "gui.h"
#include "pi.h"
#include "scalar.h"
#include "sim_params.h"
#include "sine_series.h"
#include "wrappers/sdl_context.h"
#include "wrappers/sdl_imgui.h"
#include "wrappers/sdl_imgui_context.h"
#include <Eigen/Dense>
#include <absl/strings/str_format.h>
#include <array>
#include <glad/glad.h>
#include <implot.h>
#include <iostream>

// literals for use with switch states
constexpr int LOW = 0;
constexpr int HIGH = 1;
constexpr int OFF = 2;

Scalar get_back_emf(const Eigen::Matrix<Scalar, 5, 1>& normalized_bEmf_coeffs,
                    const Scalar angle_electrical) {
    Eigen::Matrix<Scalar, 5, 1> sines;
    generate_odd_sine_series(/*num_terms=*/sines.rows(), angle_electrical,
                             sines.data());
    return sines.dot(normalized_bEmf_coeffs);
}

void step(const SimParams& params, SimState* state) {
    state->time += params.dt;

    // apply pole voltages
    for (int n = 0; n < 3; ++n) {
        Scalar v_pole = 0;
        switch (state->switches[n]) {
        case OFF:
            // todo: derivation
            if (state->i_coils(n) > 0) {
                state->v_poles(n) = 0;
            } else {
                state->v_poles(n) = params.bus_voltage;
            }
            if (std::abs(state->i_coils(n)) > params.i_diode_active) {
                state->v_poles(n) -= params.v_diode_active;
            }
            break;
        case HIGH:
            state->v_poles(n) = params.bus_voltage;
            break;
        case LOW:
            state->v_poles(n) = 0;
            break;
        default:
            printf("Unhandled switch case!");
            exit(-1);
        }
    }

    // compute normalized back emfs
    Eigen::Matrix<Scalar, 3, 1> normalized_bEmfs;
    normalized_bEmfs << // clang-format off
        get_back_emf(params.normalized_bEmf_coeffs, state->angle_electrical),
        get_back_emf(params.normalized_bEmf_coeffs,
                     state->angle_electrical + 2 * kPI / 3),
        get_back_emf(params.normalized_bEmf_coeffs,
                     state->angle_electrical - 2 * kPI / 3); // clang-format on

    state->bEmfs = normalized_bEmfs * state->angular_vel_rotor;

    // compute neutral point voltage
    // todo: derivation
    state->v_neutral = (state->v_poles.sum() - state->bEmfs.sum()) / 3;

    for (int n = 0; n < 3; ++n) {
        state->v_phases(n) = state->v_poles(n) - state->v_neutral;
    }

    Eigen::Matrix<Scalar, 3, 1> di_dt;
    for (int n = 0; n < 3; ++n) {
        di_dt(n) = (state->v_phases(n) - state->bEmfs(n) -
                    state->i_coils(n) * params.phase_resistance) /
                   params.phase_inductance;
    }

    state->i_coils += di_dt * params.dt;

    state->torque = state->i_coils.dot(normalized_bEmfs);

    state->angular_accel_rotor = state->torque / params.inertia_rotor;
    state->angular_vel_rotor += state->angular_accel_rotor * params.dt;
    state->angle_rotor += state->angular_vel_rotor * params.dt;
    state->angle_rotor = std::fmod(state->angle_rotor, 2 * kPI);

    state->angle_electrical = state->angle_rotor * params.num_pole_pairs;
    state->angle_electrical = std::fmod(state->angle_electrical, 2 * kPI);
}

using namespace biro;

int main(int argc, char* argv[]) {
    SimParams params;
    init_sim_params(&params);

    SimState state;
    init_sim_state(&state);

    VizData viz_data;
    init_viz_data(&viz_data);

    wrappers::SdlContext sdl_context("Motor Simulator",
                                     /*width=*/1920 / 2,
                                     /*height=*/1080 / 2);

    if (sdl_context.status_ != 0) {
        return -1;
    }

    if (gladLoadGL() == 0) {
        printf("Failed to initialize OpenGL loader!\n");
        exit(-1);
    }

    wrappers::SdlImguiContext imgui_context(sdl_context.window_,
                                            sdl_context.gl_context_);

    bool quit_flag = false;
    bool should_step = false;
    while (!quit_flag) {
        quit_flag = wrappers::process_sdl_imgui_events(sdl_context.window_);
        if (quit_flag) {
            break;
        }

        wrappers::sdl_imgui_newframe(sdl_context.window_);

        run_gui(&params, &state, &should_step, &viz_data);
        if (should_step) {
            for (int i = 0; i < params.step_multiplier; ++i) {
                step(params, &state);
            }
        }

        ImGui::ShowDemoWindow();
        ImPlot::ShowDemoWindow();

        ImGui::Render();
        int display_w, display_h;
        SDL_GetWindowSize(sdl_context.window_, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.0, 0.0, 0.0, 0.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(sdl_context.window_);
    }

    step(params, &state);
    return 0;
}
