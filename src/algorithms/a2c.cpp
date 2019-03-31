#include <memory>

#include <torch/torch.h>

#include "cpprl/algorithms/a2c.h"
#include "cpprl/algorithms/algorithm.h"
#include "cpprl/model/mlp_base.h"
#include "cpprl/model/policy.h"
#include "cpprl/storage.h"
#include "cpprl/spaces.h"
#include "third_party/doctest.h"

namespace cpprl
{
A2C::A2C(Policy &policy,
         float value_loss_coef,
         float entropy_coef,
         float learning_rate,
         float epsilon,
         float alpha,
         float max_grad_norm)
    : policy(policy),
      value_loss_coef(value_loss_coef),
      entropy_coef(entropy_coef),
      max_grad_norm(max_grad_norm),
      optimizer(std::make_unique<torch::optim::RMSprop>(
          policy->parameters(),
          torch::optim::RMSpropOptions(learning_rate)
              .eps(epsilon)
              .alpha(alpha))) {}

std::vector<UpdateDatum> A2C::update(RolloutStorage &rollouts)
{
    auto full_obs_shape = rollouts.get_observations().sizes();
    std::vector<int64_t> obs_shape(full_obs_shape.begin() + 2,
                                   full_obs_shape.end());
    obs_shape.insert(obs_shape.begin(), -1);
    auto action_shape = rollouts.get_actions().size(-1);
    auto rewards_shape = rollouts.get_rewards().sizes();
    int num_steps = rewards_shape[0];
    int num_processes = rewards_shape[1];

    auto evaluate_result = policy->evaluate_actions(
        rollouts.get_observations().slice(0, 0, -1).view(obs_shape),
        rollouts.get_hidden_states()[0].view({-1, policy->get_hidden_size()}),
        rollouts.get_masks().slice(0, 0, -1).view({-1, 1}),
        rollouts.get_actions().view({-1, action_shape}));

    auto values = evaluate_result[0].view({num_steps, num_processes, 1});
    auto action_log_probs = evaluate_result[1].view(
        {num_steps, num_processes, 1});

    auto advantages = rollouts.get_returns().slice(0, 0, -1) - values;
    auto value_loss = advantages.pow(2).mean();

    auto action_loss = -(advantages.detach() * action_log_probs).mean();

    optimizer->zero_grad();
    auto loss = (value_loss * value_loss_coef +
                 action_loss -
                 evaluate_result[2] * entropy_coef);
    loss.backward();

    optimizer->step();

    return {{"Value loss", value_loss.item().toFloat()},
            {"Action loss", action_loss.item().toFloat()},
            {"Entropy", evaluate_result[2].item().toFloat()}};
}

TEST_CASE("A2C")
{
    auto base = std::make_shared<MlpBase>(2, false, 10);
    ActionSpace space{"Discrete", {2}};
    Policy policy(space, base);
    RolloutStorage storage(3, 2, {2}, space, 10);
    A2C a2c(policy, 1.0, 1e-7, 0.1);

    SUBCASE("update() learns basic game")
    {
        // The game is: If the input is {1, 0} action 0 gets a reward, and for
        // {0, 1} action 1 gets a reward.
        std::vector<float> observation_vec{1, 0};
        auto pre_game_probs = policy->get_probs(
            torch::from_blob(observation_vec.data(), {2}).expand({2, 2}),
            torch::zeros({2, 10}),
            torch::ones({2, 1}));

        for (int i = 0; i < 10; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                int target_action;
                if (torch::randint(2, {1}).item().toBool())
                {
                    observation_vec = {1, 0};
                    target_action = 0;
                }
                else
                {
                    observation_vec = {0, 1};
                    target_action = 1;
                }
                auto observation = torch::from_blob(observation_vec.data(), {2});

                std::vector<torch::Tensor> act_result;
                {
                    torch::NoGradGuard no_grad;
                    act_result = policy->act(observation.expand({2, 2}),
                                             torch::Tensor(),
                                             torch::ones({2, 1}));
                }
                auto actions = act_result[1];

                float rewards_array[2];
                for (int process = 0; process < actions.size(0); ++process)
                {
                    if (actions[process].item().toInt() == target_action)
                    {
                        rewards_array[process] = 1;
                    }
                    else
                    {
                        rewards_array[process] = 0;
                    }
                }
                auto rewards = torch::from_blob(rewards_array, {2, 1});
                storage.insert(observation,
                               torch::zeros({2, 10}),
                               actions,
                               act_result[2],
                               act_result[0],
                               rewards,
                               torch::ones({2, 1}));
            }

            torch::Tensor next_value;
            {
                torch::NoGradGuard no_grad;
                next_value = policy->get_values(
                                       storage.get_observations()[-1],
                                       storage.get_hidden_states()[-1],
                                       storage.get_masks()[-1])
                                 .detach();
            }
            storage.compute_returns(next_value, false, 0.9, 0.9);

            a2c.update(storage);
            storage.after_update();
        }
        observation_vec = {1, 0};
        auto post_game_probs = policy->get_probs(
            torch::from_blob(observation_vec.data(), {2}).expand({2, 2}),
            torch::zeros({2, 10}),
            torch::ones({2, 1}));

        CHECK(post_game_probs[0][0].item().toDouble() >
              pre_game_probs[0][0].item().toDouble());
        CHECK(post_game_probs[0][1].item().toDouble() <
              pre_game_probs[0][1].item().toDouble());
    }
}
}