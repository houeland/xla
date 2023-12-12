#ifndef XLA_CLIENT_COMPUTATION_CLIENT_H_
#define XLA_CLIENT_COMPUTATION_CLIENT_H_

#include <ATen/Tensor.h>
#include <torch/csrc/lazy/backend/backend_data.h>
#include <torch/csrc/lazy/backend/lowering_context.h>
#include <torch/csrc/lazy/core/hash.h>
#include <torch/csrc/lazy/core/shape.h>
#include <torch/csrc/lazy/core/util.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/types/optional.h"
#include "absl/types/span.h"
#include "torch_xla/csrc/device.h"
#include "torch_xla/csrc/runtime/debug_macros.h"
#include "torch_xla/csrc/runtime/metrics.h"
#include "torch_xla/csrc/runtime/tensor_source.h"
#include "torch_xla/csrc/runtime/types.h"
#include "torch_xla/csrc/runtime/util.h"
#include "xla/client/xla_computation.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/literal_util.h"
#include "xla/types.h"

namespace torch_xla {
namespace runtime {

// Forward declare XlaCoordinator to avoid logging macro redefinition from the
// transitively included PJRT header.
// TODO(jonbolin): We need a way to ensure the right macros are included
// regardless of the import order.
class XlaCoordinator;

// Somehow the compiler doesn't allow type that has default member being
// used as a default parameter in a method defined in the same scope.
// Therefore, ClientExecuteOptions is defined here instead of within
// ComputationClient.
struct ClientExecuteOptions {
  bool explode_tuple{true};
};

class ComputationClient {
 public:
  class Data : public torch::lazy::BackendData {
   public:
    // TODO set Device and torch::lazy_shape correctly
    Data(std::string device, xla::Shape shape)
        : torch::lazy::BackendData(ParseDeviceString(device),
                                   torch::lazy::Shape()),
          xla_device_(device),
          xla_shape_(std::move(shape)) {}

    virtual ~Data() {}

    const std::string& device() const { return xla_device_; }

    const xla::Shape& shape() const { return xla_shape_; }

    virtual std::string ToString() const = 0;

    virtual bool HasSharding() const = 0;

    virtual xla::OpSharding GetSharding() const = 0;

   private:
    std::string xla_device_;
    xla::Shape xla_shape_;
  };

  using DataPtr = std::shared_ptr<Data>;

  // There are 4 different Computation class being used here
  // 1. torch::lazy::Computation represent a general computation from LTC
  // perspective.
  // 2. runtime::ComputationClient::Computation represent a computation from the
  // ComputationClient perspective. It wraps a xla::XlaComputation and a vector
  // of device.
  // 3. xla::XlaComputation represent a xla computation, it is generated by the
  // xla compiler.
  // 4. xla::PjRtComputationClient::PjRtComputation which inherits from
  // runtime::ComputationClient::Computation and contains a handle to represent
  // the compiled program.
  class Computation : public torch::lazy::Computation {
   public:
    // Our Computation is being used for 3 different purpose.
    // 1. To represent a xla computation build by xla_op_builder, in which case
    //    we would need the name and hash. Computation would be a wrapper around
    //    a runtime::ComputationClient::Computation.
    //    runtime::ComputationClient::Computation::devices_ would be empty.
    // 2. To represent a computation built by syncTensor and needs to be
    //    compiled.
    //    ...
    Computation(std::string name, xla::XlaComputation computation,
                std::vector<std::string> devices = {})
        : name_(name),
          computation_(std::move(computation)),
          devices_(std::move(devices)) {
      program_shape_ = ConsumeValue(computation_.GetProgramShape());
      hash_ =
          torch::lazy::MHash(name, computation_.proto().SerializeAsString());
    }

    Computation(std::string name, xla::XlaComputation computation,
                torch::lazy::BackendDevice device)
        : Computation(name, std::move(computation), {device.toString()}) {}

    // ...
    // 3. To represent a computation that is already compiled. In this case
    //    name_ and hash_ are not required. Computation will be a wrapper around
    //    an executable, PjRtComputationClient::PjRtComputation in our case. It
    //    is not ideal to use same class for 3 different purposes but this is
    //    the path took by upstream ltc.
    Computation(xla::XlaComputation computation,
                std::vector<std::string> devices)
        : Computation("", std::move(computation), std::move(devices)) {}

    Computation(xla::XlaComputation computation)
        : Computation(std::move(computation), {}) {}

    virtual ~Computation() {}

    const std::string& name() const { return name_; }

    std::string get_device_string() const {
      // Assume that a xla_client_computation_ only contains one device for now.
      XLA_CHECK_EQ(devices().size(), 1);
      return devices()[0];
    }

    const xla::XlaComputation& computation() const {
      if (computation_moved_) {
        XLA_ERROR() << "Computation has been moved\n";
      }
      return computation_;
    }

    // We don't want to make a copy when passing computation_ to the runtime.
    // Class member will be accessed as const& and `xla::XlaComputation`
    // explictly delete its const& copy constructor so we have to const cast
    // here.
    xla::XlaComputation move_computation() {
      if (computation_moved_) {
        XLA_ERROR() << "Compuation has been moved\n";
      }
      computation_moved_ = true;
      return std::move(const_cast<Computation*>(this)->computation_);
    }

    const xla::ProgramShape& program_shape() const { return program_shape_; }

    const torch::lazy::hash_t& hash() const { return hash_; }

    int parameters_size() const override {
      return program_shape().parameters_size();
    }

    const std::vector<torch::lazy::Shape>& parameter_shapes() const override {
      XLA_ERROR() << "Unimplemented";
    }

    const std::vector<std::string>& parameter_names() const override {
      return program_shape().parameter_names();
    }

    const torch::lazy::Shape& result_shape() const override {
      XLA_ERROR() << "Unimplemented";
    }

    const std::vector<std::string>& devices() const { return devices_; }

    const std::string to_string() const override {
      xla::HloModuleConfig hlo_config(program_shape());
      std::unique_ptr<xla::HloModule> module = ConsumeValue(
          xla::HloModule::CreateFromProto(computation().proto(), hlo_config));
      return module->ToString();
    }

   private:
    xla::XlaComputation computation_;
    xla::ProgramShape program_shape_;
    std::vector<std::string> devices_;
    bool computation_moved_ = false;

    torch::lazy::hash_t hash_;
    std::string name_;
  };

  using ComputationPtr = std::shared_ptr<Computation>;

  // TODO(wcromar): Should CompileInstance still exist? Should it be a subclass
  // of torch::lazy::Computation?
  struct CompileInstance {
    CompileInstance() = default;
    CompileInstance(xla::XlaComputation computation,
                    std::string compilation_device,
                    std::vector<std::string> devices,
                    const xla::Shape* output_shape,
                    bool parameter_is_tupled_arguments = false,
                    bool is_sharded = false,
                    bool allow_spmd_sharding_propagation_to_output = true)
        : computation(std::move(computation)),
          compilation_device(std::move(compilation_device)),
          devices(std::move(devices)),
          output_shape(output_shape),
          parameter_is_tupled_arguments(parameter_is_tupled_arguments),
          is_sharded(is_sharded),
          allow_spmd_sharding_propagation_to_output(
              allow_spmd_sharding_propagation_to_output) {}

    xla::XlaComputation computation;
    std::string compilation_device;
    std::vector<std::string> devices;
    const xla::Shape* output_shape = nullptr;
    bool parameter_is_tupled_arguments;
    bool is_sharded;
    bool allow_spmd_sharding_propagation_to_output;
  };

  struct ExecuteComputationOptions : public ClientExecuteOptions {};

  struct ExecuteReplicatedOptions : public ClientExecuteOptions {};

  struct MemoryInfo {
    int64_t kb_free = 0;
    int64_t kb_total = 0;
  };

  virtual ~ComputationClient() {}

  // Creates a Data object with no actual device handle in it. The device handle
  // will be populated in an asynchrounous fashion.
  virtual DataPtr CreateDataPlaceholder(std::string device,
                                        xla::Shape shape) = 0;

  // Returns data shards. We expect this to be called on PjRtShardedData to
  // retrieve the shards. If other data type is passed, it returns the input
  // wrapped inside a vector.
  virtual std::vector<DataPtr> GetDataShards(DataPtr data) = 0;

  // Returns data shard at a given index.
  virtual DataPtr GetDataShard(DataPtr data, size_t index) = 0;

  // Returns wrapped data shards as PjRtShardedData.
  virtual DataPtr WrapDataShards(const std::vector<DataPtr>& shards,
                                 std::string device, xla::Shape shape,
                                 xla::OpSharding sharding) = 0;

  // Returns OpSharding attached to PjRtShardedData. The returned optional
  // structure will be empty if there is no sharding, like with PjRtData.
  virtual std::optional<xla::OpSharding> GetDataSharding(DataPtr handle) = 0;

  // Transfers local tensor values to the TPU devices and fetches the handles.
  virtual std::vector<DataPtr> TransferToServer(
      absl::Span<const std::shared_ptr<const TensorSource>> tensors) = 0;

  // Transfers local sharded tensor values to the TPU devices and returns a
  // `PjRtShardedData`.
  virtual DataPtr TransferShardsToServer(
      absl::Span<const std::shared_ptr<const TensorSource>> tensor_shards,
      std::string device, xla::Shape shape, xla::OpSharding sharding) = 0;

  // Copies `data->buffer` to `dst` device buffer.
  virtual DataPtr CopyToDevice(DataPtr data, std::string dst) = 0;

  // Reads the tensor literal values stored at TPU server sites, behind the
  // supplied handles.
  // Note: `TransferFromServer` call will block until the `DataPtrs` are ready
  // if they were created by `TransferToServer` or `Execute*`. Calling this from
  // python while holding the GIL can cause deadlocks!
  virtual std::vector<xla::Literal> TransferFromServer(
      absl::Span<const DataPtr> handles) = 0;

  // Compiles a set of computations.
  virtual std::vector<ComputationPtr> Compile(
      std::vector<CompileInstance> instances) = 0;

  // Serialize a computation to a string.
  virtual std::string SerializeComputation(
      const ComputationPtr computation) = 0;

  // Deserialize a string resulting from SerializeComputation back to a
  // Computation. If the deserialization fails, nullptr is returned.
  virtual ComputationPtr DeserializeComputation(
      const std::string& serialized) = 0;

  // Returns a hash of the current compilation environment.
  virtual torch::lazy::hash_t HashCompilationEnv() = 0;

  // Executes computation with arguments and returns the result.
  // The passed device must match the common device of the arguments Data.
  // If options.explode_tuple is true, the output tuple will be decomposed into
  // its single elements.
  virtual std::vector<DataPtr> ExecuteComputation(
      const Computation& computation, absl::Span<const DataPtr> arguments,
      const std::string& device,
      const ExecuteComputationOptions& options =
          ExecuteComputationOptions{}) = 0;

  // Executes the computation on multiple local devices in parallel.
  // Each argument to the executable is expected to be sharded in the same order
  // as `devices`. If options.explode_tuple is true, the output tuples will be
  // decomposed into their single elements. Returns a vector of outputs, each
  // of which is sharded in the same order as `devices`.
  virtual std::vector<DataPtr> ExecuteReplicated(
      const Computation& computation, absl::Span<const DataPtr> arguments,
      absl::Span<const std::string> devices,
      const ExecuteReplicatedOptions& options) = 0;

  virtual std::string GetDefaultDevice() const = 0;

  virtual size_t GetNumDevices() const = 0;

  virtual std::vector<std::string> GetLocalDevices() const = 0;

  virtual std::vector<std::string> GetAllDevices() const = 0;

  virtual int GetProcessIndex() const = 0;

  virtual int GetNumProcesses() const = 0;

  using DeviceAttribute =
      std::variant<std::string, bool, int64_t, std::vector<int64_t>, float>;

  virtual const absl::flat_hash_map<
      std::string, torch_xla::runtime::ComputationClient::DeviceAttribute>&
  GetDeviceAttributes(const std::string& device) = 0;

  virtual void SetReplicationDevices(
      std::shared_ptr<std::vector<std::string>> devices) = 0;

  virtual std::shared_ptr<std::vector<std::string>> GetReplicationDevices() = 0;

  virtual std::map<std::string, Metric> GetMetrics() const = 0;

  virtual MemoryInfo GetMemoryInfo(const std::string& device) = 0;

  // Block until pass in devices' async operation are finished. If empty, all
  // the local devices will be waited for.
  virtual void WaitDeviceOps(absl::Span<const std::string> devices) = 0;

  // Check whether the XlaCoordinator has been initialized.
  virtual bool CoordinatorInitialized() const = 0;

  // Initialize the XlaCoordinator for the runtime.
  virtual void InitializeCoordinator(int global_rank, int world_size,
                                     std::string master_addr,
                                     std::string port) = 0;

  // Return the XlaCoordinator for the runtime.
  virtual XlaCoordinator& GetCoordinator() = 0;

  // Utility API around the vector based Compile() API to compile a single
  // computation.
  ComputationPtr Compile(xla::XlaComputation computation,
                         std::string compilation_device,
                         std::vector<std::string> devices,
                         const xla::Shape* output_shape);

  // Retrieves the set of devices to be passed to the computation client
  // Compile() API. If the devices array is empty, a vector with the single
  // device will be returned. Otherwise a vector with the devices content will
  // be returned.
  std::vector<std::string> GetCompilationDevices(
      const std::string& device, absl::Span<const std::string> devices);

  // Retrieves the ordinal number out of a device string. This is the number
  // after the last ':' character of the device string.
  static int64_t GetDeviceOrdinal(const std::string& device);

  static void RegisterPjRtPlugin(std::string name, std::string library_path);

  static std::optional<std::string> GetPjRtPluginPath(
      const std::string& device_type);

 protected:
  static constexpr auto spmd_device_str = "SPMD:0";

  // Metrics common to all client interfaces.
  static metrics::Metric* TransferToServerMetric();
  static metrics::Metric* TransferToServerTransformMetric();
  static metrics::Metric* TransferFromServerMetric();
  static metrics::Metric* CompileMetric();
  static metrics::Metric* ExecuteMetric();
  static metrics::Metric* ExecuteReplicatedMetric();
  static metrics::Metric* ExecuteParallelMetric();
  static metrics::Metric* ExecuteChainedMetric();
  static metrics::Metric* DeconstructTupleMetric();
  static metrics::Counter* CreateAsyncDataHandlesCounter();
  static metrics::Counter* CreateDataHandlesCounter();
  static metrics::Counter* ReleaseDataHandlesCounter();
  static metrics::Counter* DestroyDataHandlesCounter();
  static metrics::Metric* ReleaseDataHandlesTimeMetric();
  static metrics::Counter* CreateCompileHandlesCounter();
  static metrics::Counter* ReleaseCompileHandlesCounter();
  static metrics::Counter* DestroyCompileHandlesCounter();
  static metrics::Metric* ReleaseCompileHandlesTimeMetric();
  static metrics::Counter* StableHloCompileCounter();
  static metrics::Metric* InboundDataMetric();
  static metrics::Metric* OutboundDataMetric();
};

}  // namespace runtime
}  // namespace torch_xla

#endif  // XLA_CLIENT_COMPUTATION_CLIENT_H_
