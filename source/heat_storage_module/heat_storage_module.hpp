/*******************************************************************
******************       CFD SOLVER 2D        **********************
********************************************************************/

#pragma once

#include "../control/experiment.hpp"
#include "../common/vect.hpp"
#include "../hydro2dmpi/mesh.hpp"
#include "../hydro2dmpi/mesh1d.hpp"
#include "../hydro2dmpi/output.hpp"
#include "../hydro2dmpi/linear.hpp"
#include "../hydro2dmpi/solver.hpp"
#include <memory>
#include <utility>

// TODO: Change parameters on events (e.g. certain time moments)

namespace heat_storage_module
{

template <class Vect>
Vect GetVect(const column<double>& v);

template <>
geom::Vect<double, 1> GetVect<geom::Vect<double, 1>>(const column<double>& v) {
  return geom::Vect<double, 1>(v[0]);
}


template <>
geom::Vect<float, 1> GetVect<geom::Vect<float, 1>>(const column<double>& v) {
  return geom::Vect<float, 1>(float(v[0]));
}

template <class Mesh>
class HeatStorage : public solver::UnsteadySolver {
 public:
  static constexpr size_t dim = Mesh::dim;
  using Scal = typename Mesh::Scal;
  using MIdx = typename Mesh::MIdx;
  using Direction = typename Mesh::Direction;
  using Vect = typename Mesh::Vect;

  using IntIdx = geom::IntIdx;
  using IdxCell = geom::IdxCell;
  using IdxFace = geom::IdxFace;
  using IdxNode = geom::IdxNode;

  using Layers = solver::Layers;

  template <class T>
  using FieldCell = geom::FieldCell<T>;
  template <class T>
  using FieldFace = geom::FieldFace<T>;
  template <class T>
  using FieldNode = geom::FieldNode<T>;

  using FuncTX = std::function<Scal(Scal, Scal)>;

  static FieldCell<Scal> Evaluate(FuncTX func, double t, const Mesh& mesh) {
    FieldCell<Scal> res(mesh);
    for (auto idxcell : mesh.Cells()) {
      Scal x = mesh.GetCenter(idxcell)[0];
      res[idxcell] = func(t, x);
    }
    return res;
  }

  class TesterMms {
   public:
    struct Entry {
      Scal diff_prev;
      Scal error;
      Scal h;
      Mesh mesh;
      FieldCell<Scal> fc_fluid_temperature;
      FieldCell<Scal> fc_exact_fluid_temperature;
      std::shared_ptr<HeatStorage> solver;
    };
    TesterMms(size_t num_cells_initial, size_t num_stages, size_t factor,
              Scal domain_length, size_t num_steps, double time_step,
              Scal step_threshold,
              Scal fluid_velocity, Scal conductivity, Scal Tleft,
              FuncTX func_rhs_fluid, FuncTX func_exact_fluid_temperature) {
      std::ofstream stat("mms_statistics.dat");
      stat.precision(20);
      stat << "num_cells error diff dt num_steps step_diff" << std::endl;
      
      std::string field_name_prefix = "field_T_fluid_";

      for (size_t num_cells = num_cells_initial, i = 0;
          i < num_stages; ++i, num_cells *= factor) {
        series_.emplace_back();
        Entry& entry = series_.back();

        // Create mesh
        Mesh& mesh = entry.mesh;
        MIdx mesh_size(num_cells);
        geom::Rect<Vect> domain(Vect(0.), Vect(domain_length));
        geom::InitUniformMesh(mesh, domain, mesh_size);

        FieldCell<Scal> fc_rhs_fluid(mesh, 0.), fc_rhs_solid(mesh, 0.);
        fc_rhs_fluid = Evaluate(func_rhs_fluid, 0., mesh);
        entry.solver = std::make_shared<HeatStorage>(mesh, time_step, fluid_velocity,
                                                     conductivity, conductivity,
                                                     Tleft, Tleft, 0., 0.,
                                                     &fc_rhs_fluid, &fc_rhs_solid);
        auto& solver = *entry.solver;
        size_t actual_num_steps = num_steps;
        for (size_t n = 0; n < num_steps; ++n) {
          solver.StartStep();
          solver.CalcStep();
          solver.FinishStep();
          if (solver::CalcDiff(solver.GetFluidTemperature(Layers::time_curr),
                               solver.GetFluidTemperature(Layers::time_prev), mesh) < step_threshold) {
            actual_num_steps = n;
            break;
          }
        }

        entry.fc_fluid_temperature = solver.GetFluidTemperature();
        entry.diff_prev = series_.empty() ? 0. :
            solver::CalcDiff(entry.fc_fluid_temperature,
                             GetInterpolated(series_.back().fc_fluid_temperature, series_.back().mesh, mesh),
                             mesh);

        entry.fc_exact_fluid_temperature = Evaluate(func_exact_fluid_temperature, 0., mesh);
        entry.error = solver::CalcDiff(entry.fc_exact_fluid_temperature,
                                       entry.fc_fluid_temperature,
                                       mesh);
        
        const Scal step_diff = 
            solver::CalcDiff(solver.GetFluidTemperature(Layers::time_curr),
                             solver.GetFluidTemperature(Layers::time_prev), mesh);

        stat << num_cells << ' ' << entry.error << ' ' << entry.diff_prev 
            << time_step << ' ' << actual_num_steps << ' ' << step_diff << std::endl;

        solver.WriteField(solver.GetFluidTemperature(),
                          field_name_prefix + IntToStr(num_cells) + ".dat");
      }

      // Write the exact solution on the finest mesh
      if (!series_.empty()) {
        auto& entry = series_.back();
        entry.solver->WriteField(entry.fc_exact_fluid_temperature,
                                 field_name_prefix + "exact" + ".dat");
      }
    }
    const std::vector<Entry>& GetSeries() const {
      return series_;
    }

   private:
    std::vector<Entry> series_;
  };

  class Scheduler {
   public:
    enum class State { Charging, Idle, Discharging };

    Scheduler(double d1, double d2, double d3, double d4)
        : d1_(d1), d2_(d2), d3_(d3), d4_(d4) {}

    State GetState(double t) {
      double cycle_duration = d1_ + d2_ + d3_ + d4_;
      size_t cycle = static_cast<size_t>(t / cycle_duration);
      double offset = t - cycle * cycle_duration;
      return offset < d1_ ? State::Charging :
          offset < d1_ + d2_ ? State::Idle :
          offset < d1_ + d2_ + d3_ ? State::Discharging : State::Idle;
    }
    size_t GetStateIdx(double t) {
      switch (GetState(t)) {
        case State::Charging:
          return 1;
        case State::Discharging:
          return 2;
        case State::Idle:
          return 3;
        default:
          assert(false);
      }
    }
   private:
    double d1_, d2_, d3_, d4_;
  };

  HeatStorage(const Mesh& mesh, double time_step,
              double fluid_velocity, double conductivity_fluid, double conductivity_solid,
              double temperature_hot, double temperature_cold,
              double exchange_fluid, double exchange_solid,
              const FieldCell<Scal>* p_fc_rhs_fluid, const FieldCell<Scal>* p_fc_rhs_solid)
      : UnsteadySolver(0., time_step),
        mesh(mesh), fluid_velocity_(fluid_velocity),
        conductivity_fluid_(conductivity_fluid), conductivity_solid_(conductivity_solid),
        temperature_hot_(temperature_hot), temperature_cold_(temperature_cold),
        exchange_fluid_(exchange_fluid), exchange_solid_(exchange_solid),
        p_fc_rhs_fluid_(p_fc_rhs_fluid), p_fc_rhs_solid_(p_fc_rhs_solid)
        {

    //,
    //      scheduler_(P_double["duration_1"], P_double["duration_2"],
    //                 P_double["duration_3"], P_double["duration_4"])

    // Init fields
    fc_temperature_fluid_.time_curr.Reinit(mesh, temperature_cold_);
    fc_temperature_fluid_.time_prev.Reinit(mesh, temperature_cold_);
    fc_temperature_solid_.time_curr.Reinit(mesh, temperature_cold_);
    fc_temperature_solid_.time_prev.Reinit(mesh, temperature_cold_);
  }
  const FieldCell<Scal>& GetFluidTemperature(Layers layer) const {
    return fc_temperature_fluid_.Get(layer);
  }
  const FieldCell<Scal>& GetFluidTemperature() const {
    return fc_temperature_fluid_.time_curr;
  }
  const FieldCell<Scal>& GetSolidTemperature(Layers layer) const {
    return fc_temperature_solid_.Get(layer);
  }
  const FieldCell<Scal>& GetSolidTemperature() const {
    return fc_temperature_solid_.time_curr;
  }
  static Scal GetInterpolated(Scal x, Scal x_left, Scal x_right, Scal u_left, Scal u_right) {
    return ((x - x_left) * u_right + (x_right - x) * u_left) / (x_right - x_left);
  }
  // Applies linear interpolation on 1D mesh
  static FieldCell<Scal> GetInterpolated(const FieldCell<Scal>& fc_src, const Mesh& mesh_src, const Mesh& mesh_dest) {
	  static_assert(Mesh::dim == 1, "HeatStorage::GetInterpolated() requires a 1D mesh");
	  FieldCell<Scal> res(mesh_dest);
	  IdxCell idx_left_src = IdxCell(0);
	  IdxCell idx_right_src = idx_left_src;
	  for (IdxCell idx_dest : mesh_dest.Cells()) {
	    Scal x_dest = mesh_dest.GetCenter(idx_dest)[0];
	    while (mesh_src.GetCenter(idx_right_src)[0] < x_dest) {
	      IdxCell idx_new_src = mesh_src.GetNeighbourCell(idx_right_src, 1);
	      if (idx_new_src.IsNone()) {
	        break;
	      }
	      idx_left_src = idx_right_src;
	      idx_right_src = idx_new_src;
	    }
	    res[idx_dest] = GetInterpolated(x_dest, mesh_src.GetCenter(idx_left_src)[0], mesh_src.GetCenter(idx_right_src)[0],
	                                    fc_src[idx_left_src], fc_src[idx_right_src]);
	  }
	  return res;
  }
  void WriteField(const FieldCell<Scal>& fc_u, std::string filename) const {
    output::Content content = {
        std::make_shared<output::EntryFunction<Scal, IdxCell, Mesh>>(
            "x", mesh,
            [this](IdxCell idx) { return mesh.GetCenter(idx)[0]; })
        , std::make_shared<output::EntryFunction<Scal, IdxCell, Mesh>>(
            "u", mesh,
            [&](IdxCell idx) { return fc_u[idx]; })
    };
    output::SessionPlain<Mesh> session(content, filename, mesh);
    session.Write(0., "field");
  }
  void CalcStep() {
    auto& Tf = fc_temperature_fluid_.time_prev;
    auto& Tf_new = fc_temperature_fluid_.time_curr;
    std::swap(Tf, Tf_new);
    auto& Ts = fc_temperature_solid_.time_prev;
    auto& Ts_new = fc_temperature_solid_.time_curr;
    std::swap(Ts, Ts_new);

    const Scal h = mesh.GetVolume(IdxCell(0)); // uniform mesh assumed
    const Scal dt = this->GetTimeStep();
    const Scal uf = fluid_velocity_;
    const Scal alpha_f = conductivity_fluid_;
    const Scal alpha_s = conductivity_solid_;
    const Scal T_in = temperature_hot_;

    // Equation: dT/dt + div(fluxes) = 0
    FieldFace<Scal> ff_flux_fluid(mesh, 0.);
    FieldFace<Scal> ff_flux_solid(mesh, 0.);
    for (IdxFace idxface : mesh.Faces()) {
      IdxCell cm = mesh.GetNeighbourCell(idxface, 0);
      IdxCell cp = mesh.GetNeighbourCell(idxface, 1);
      auto& flux_fluid = ff_flux_fluid[idxface];
      auto& flux_solid = ff_flux_solid[idxface];
      if (cm.IsNone()) { // left boundary
        flux_fluid += uf * T_in;
        flux_solid += 0.;
      } else if (cp.IsNone()) { // right boundary
        flux_fluid += uf * Tf[cm];
        flux_solid += 0.;
      } else {
        // convection: first order upwind
        flux_fluid += uf * Tf[cm];
        // diffusion: central second order
        flux_fluid += -alpha_f * (Tf[cp] - Tf[cm]) / h;
        flux_solid += -alpha_s * (Ts[cp] - Ts[cm]) / h;
      }
    }

    // Time integration of flux terms
    for (IdxCell idxcell : mesh.Cells()) {
      IdxFace fm = mesh.GetNeighbourFace(idxcell, 0);
      IdxFace fp = mesh.GetNeighbourFace(idxcell, 1);
      const Scal fluxsum_fluid = ff_flux_fluid[fp] - ff_flux_fluid[fm];
      Tf_new[idxcell] = Tf[idxcell] - dt / h * fluxsum_fluid;
      const Scal fluxsum_solid = ff_flux_solid[fp] - ff_flux_solid[fm];
      Ts_new[idxcell] = Ts[idxcell] - dt / h * fluxsum_solid;
    }

    // Time integration of source terms
    if (p_fc_rhs_fluid_) {
      for (IdxCell idxcell : mesh.Cells()) {
        Tf_new[idxcell] += dt * (*p_fc_rhs_fluid_)[idxcell];
      }
    }
    if (p_fc_rhs_solid_) {
      for (IdxCell idxcell : mesh.Cells()) {
        Ts_new[idxcell] += dt * (*p_fc_rhs_solid_)[idxcell];
      }
    }

    // Implicit heat exchange

  }
  const Mesh& GetMesh() const { return mesh; }

 private:
  const Mesh& mesh;
  solver::LayersData<FieldCell<Scal>>
  fc_temperature_fluid_, fc_temperature_solid_;
  Scal fluid_velocity_;
  Scal conductivity_fluid_, conductivity_solid_;
  Scal temperature_hot_, temperature_cold_;
  Scal exchange_fluid_, exchange_solid_;
  const FieldCell<Scal>* p_fc_rhs_fluid_;
  const FieldCell<Scal>* p_fc_rhs_solid_;
};

template <class Mesh>
class hydro : public TModule
{
  static constexpr size_t dim = Mesh::dim;
  using Scal = typename Mesh::Scal;
  using MIdx = typename Mesh::MIdx;
  using Direction = typename Mesh::Direction;
  using Vect = typename Mesh::Vect;

  using IntIdx = geom::IntIdx;
  using IdxCell = geom::IdxCell;
  using IdxFace = geom::IdxFace;
  using IdxNode = geom::IdxNode;

  using Layers = solver::Layers;

  template <class T>
  using FieldCell = geom::FieldCell<T>;
  template <class T>
  using FieldFace = geom::FieldFace<T>;
  template <class T>
  using FieldNode = geom::FieldNode<T>;

  // TODO: pack common declarations somewhere (Scal, Vect, FieldCell, etc)
 public:
  hydro(TExperiment* _ex);
  ~hydro() {}
  void step();
  void write_results(bool force=false);

  output::Content content, content_scalar;
  std::shared_ptr<output::Session> session, session_scalar;

  double last_frame_time_;
  double last_frame_scalar_time_;

  std::shared_ptr<HeatStorage<Mesh>> solver_;
  Mesh mesh;
};

template <class Mesh>
hydro<Mesh>::hydro(TExperiment* _ex)
    : TExperiment_ref(_ex), TModule(_ex)
{
  P_int.set("last_s", 0);
  P_double.set("last_R", 0);
  P_double.set("last_Rn", 0);

  P_int.set("s_sum", 0);
  P_int.set("s_max", 0);
  P_int.set("s", 0);
  P_int.set("current_frame", 0);
  P_int.set("current_frame_scalar", 0);

  geom::Rect<Vect> domain(GetVect<Vect>(P_vect["A"]),
                          GetVect<Vect>(P_vect["B"]));

  // Create mesh
  MIdx mesh_size(P_int["Nx"]);
  geom::InitUniformMesh(mesh, domain, mesh_size);

  solver_ = std::make_shared<HeatStorage<Mesh>>(
      mesh, dt,
      P_double["uf"],
      P_double["alpha_fluid"], P_double["alpha_solid"],
      P_double["T_hot"], P_double["T_cold"],
      P_double["exchange"], P_double["exchange"],
      nullptr, nullptr);

  P_int.set("cells_number", static_cast<int>(mesh.GetNumCells()));

  content = {
      std::make_shared<output::EntryFunction<Scal, IdxCell, Mesh>>(
          "x", mesh,
          [this](IdxCell idx) { return mesh.GetCenter(idx)[0]; })
      , std::make_shared<output::EntryFunction<Scal, IdxCell, Mesh>>(
          "Tf", mesh,
          [this](IdxCell idx) { return solver_->GetFluidTemperature()[idx]; })
      , std::make_shared<output::EntryFunction<Scal, IdxCell, Mesh>>(
          "Ts", mesh,
          [this](IdxCell idx) { return solver_->GetSolidTemperature()[idx]; })

  };

  auto P = [this](std::string entry, std::string parameter) {
    return std::make_shared<output::EntryScalarFunction<Scal>>(
        entry, [this, parameter](){ return static_cast<Scal>(P_double[parameter]); });
  };
  auto Pint = [this](std::string entry, std::string parameter) {
    return std::make_shared<output::EntryScalarFunction<Scal>>(
        entry, [this, parameter](){ return static_cast<Scal>(P_int[parameter]); });
  };
  content_scalar = { P("time", "t"), Pint("n", "n")
      //, std::make_shared<output::EntryScalarFunction<Scal>>(
      //    "status", [this](){ return static_cast<Scal>(scheduler_.GetStateIdx(P_double["t"])); })
  };

  if (!P_string.exist(_plt_title)) {
    P_string.set(_plt_title, P_string[_exp_name]);
  }
  if (!P_string.exist("filename_field")) {
    P_string.set("filename_field", P_string[_exp_name] + ".field.dat");
  }
  if (!P_string.exist("filename_scalar")) {
    P_string.set("filename_scalar", P_string[_exp_name] + ".scalar.dat");
  }

  session = std::make_shared<output::SessionPlain<Mesh>>(
      content, P_string["filename_field"], mesh);

  session_scalar = std::make_shared<output::SessionPlainScalar<Scal>>(
      content_scalar, P_string["filename_scalar"]);

  last_frame_time_ = 0;
  last_frame_scalar_time_ = 0;
  session->Write(0., "initial");
  session_scalar->Write();


  if (flag("MMS")) {
    const Scal uf = P_double["MMS_fluid_velocity"];
    const Scal alpha = P_double["MMS_alpha"];
    const Scal wavenumber = P_double["MMS_wavenumber"];

    using FuncTX = typename HeatStorage<Mesh>::FuncTX;
    FuncTX func_exact, func_rhs;

    const std::string mmsfunc = P_string["MMS_exact_solution"];
    if (mmsfunc == "cos(kx)") {
      func_exact = [=](Scal, Scal x) { return std::cos(x * wavenumber); };
      func_rhs = [=](Scal, Scal x) {
        return -uf * wavenumber * std::sin(x * wavenumber) +
            alpha * sqr(wavenumber) * std::cos(x * wavenumber);
      };
    } else if (mmsfunc == "cos(kx^2)") {
      func_exact = [=](Scal, Scal x) { return std::cos(sqr(x) * wavenumber); };
      func_rhs = [=](Scal, Scal x) {
        return -uf * wavenumber * 2. * x * std::sin(sqr(x) * wavenumber) +
            alpha * (sqr(wavenumber) * 4. * sqr(x) * std::cos(sqr(x) * wavenumber) +
                    wavenumber * 2. * std::sin(sqr(x) * wavenumber));
      };
    } else {
      throw std::string("Unknown MMS_exact_solution: " + mmsfunc);
    }

    typename HeatStorage<Mesh>::TesterMms tester(
        P_int["MMS_mesh_initial"],
        P_int["MMS_num_stages"],
        P_int["MMS_factor"],
        P_double["MMS_domain_length"],
        P_int["MMS_num_steps"],
        P_double["MMS_time_step"],
        P_double["MMS_step_threshold"],
        P_double["MMS_fluid_velocity"],
        P_double["MMS_alpha"],
        P_double["MMS_T_left"],
        func_rhs, func_exact);
  }
}

template <class Mesh>
void hydro<Mesh>::step() {
  ex->timer_.Push("step");

  solver_->StartStep();
  solver_->CalcStep();
  solver_->FinishStep();

  ++P_int["n"];
  ex->timer_.Pop();
}

template <class Mesh>
void hydro<Mesh>::write_results(bool force) {
  if (ecast(P_bool("no_output"))) {
    return;
  }

  const double time = P_double["t"];
  const double total_time = P_double["T"];
  const size_t max_frame_index = P_int["max_frame_index"];
  const double frame_duration = total_time / max_frame_index;

  if (force || (!ecast(P_bool("no_mesh_output")) &&
      time >= last_frame_time_ + frame_duration)) {
    last_frame_time_ = time;
    session->Write(time, "step");
    logger() << "Frame " << (P_int["current_frame"]++) << ": t=" << time;
  }

  const size_t max_frame_scalar_index = P_int["max_frame_scalar_index"];
  const double frame_scalar_duration = total_time / max_frame_scalar_index;

  if (force || time >= last_frame_scalar_time_ + frame_scalar_duration) {
    last_frame_scalar_time_ = time;
    session_scalar->Write();
    logger() << "Frame_scalar " << (P_int["current_frame_scalar"]++)
        << ": t=" << time;
  }
}

} // namespace heat_storage_module
