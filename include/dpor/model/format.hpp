#pragma once

// Human-readable rendering of exploration graphs, for use anywhere a trace
// needs to be shown or saved: terminal-execution observers, on_fatal_error
// diagnostics, and test failure dumps.

#include "dpor/model/event.hpp"
#include "dpor/model/exploration_graph.hpp"

#include <concepts>
#include <string>
#include <utility>

namespace dpor::model {

// Renders the graph's events in insertion order, one per line, using
// value_formatter (ValueT -> std::string) for message payloads. Works on any
// graph state, including the possibly mid-mutation snapshot handed to
// on_fatal_error: rf entries are rendered defensively rather than assumed
// well-formed.
template <typename ValueT, typename ValueFormatter>
[[nodiscard]] inline std::string format_graph(const ExplorationGraphT<ValueT>& graph,
                                              ValueFormatter&& value_formatter) {
  std::string out;
  const auto& rf = graph.reads_from();

  for (const auto id : graph.insertion_order()) {
    const auto& evt = graph.event(id);
    out += "event " + std::to_string(id) + " thread=" + std::to_string(evt.thread) +
           " index=" + std::to_string(evt.index) + " ";

    if (const auto* send = as_send(evt)) {
      out += "send(dest=" + std::to_string(send->destination) +
             ", val=" + value_formatter(send->value) + ")";
    } else if (const auto* recv = as_receive(evt)) {
      out += recv->is_blocking() ? "receive(" : "receive(nonblocking, ";
      const auto rf_it = rf.find(id);
      if (rf_it == rf.end()) {
        out += "unmatched)";
      } else if (rf_it->second.is_bottom()) {
        out += "bottom)";
      } else {
        const auto send_id = rf_it->second.send_id();
        out += "from=" + std::to_string(send_id);
        if (graph.is_valid_event_id(send_id)) {
          if (const auto* source = as_send(graph.event(send_id))) {
            out += ", val=" + value_formatter(source->value);
          }
        }
        out += ")";
      }
    } else if (const auto* nd = as_nondeterministic_choice(evt)) {
      out += "nd(val=" + value_formatter(nd->value) + ")";
    } else if (is_block(evt)) {
      out += "block";
    } else if (const auto* error = as_error(evt)) {
      out += "error(" + error->message + ")";
    }

    out += "\n";
  }

  return out;
}

// Convenience overload for string-convertible payloads such as the default
// model::Value.
template <typename ValueT>
  requires std::convertible_to<const ValueT&, std::string>
[[nodiscard]] inline std::string format_graph(const ExplorationGraphT<ValueT>& graph) {
  return format_graph(graph, [](const ValueT& value) { return std::string(value); });
}

}  // namespace dpor::model
