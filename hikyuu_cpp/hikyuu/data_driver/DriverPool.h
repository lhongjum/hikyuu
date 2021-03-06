/*
 *  Copyright(C) 2021 hikyuu.org
 *
 *  Create on: 2021-01-19
 *     Author: fasiondog
 */

#pragma once

#include <thread>
#include <mutex>
#include <queue>
#include <memory>
#include "../utilities/Parameter.h"

namespace hku {

template <typename DriverType>
class DriverWrap {
public:
    DriverWrap() = default;

    typedef std::shared_ptr<DriverType> DriverPtr;
    DriverWrap(const DriverPtr &driver) : m_driver(driver) {}

    DriverPtr get() {
        return m_driver;
    }

    explicit operator bool() const noexcept {
        return m_driver.get() != nullptr;
    }

private:
    DriverPtr m_driver;
};

/**
 * 驱动资源池
 * @tparam DriverType 驱动类型，要求具备 DriverType *clone() 方法
 * @ingroup DataDriver
 */
template <typename DriverType>
class DriverPool {
public:
    DriverPool() = delete;
    DriverPool(const DriverPool &) = delete;
    DriverPool &operator=(const DriverPool &) = delete;

    /**
     * 构造函数
     * @param param 驱动原型，所有权将被转移至该 pool
     * @param maxConnect 允许的最大连接数，为 0 表示不限制
     * @param maxIdleConnect 运行的最大空闲连接数，等于 0 时表示立刻释放，默认为CPU数
     */
    explicit DriverPool(const std::shared_ptr<DriverType> &prototype, size_t maxConnect = 0,
                        size_t maxIdleConnect = std::thread::hardware_concurrency())
    : m_maxSize(maxConnect),
      m_maxIdelSize(maxIdleConnect),
      m_count(0),
      m_prototype(prototype),
      m_closer(this) {}

    typedef DriverWrap<DriverType> Wrap;
    typedef std::shared_ptr<Wrap> WrapPtr;
    typedef std::shared_ptr<DriverType> DriverPtr;

    /**
     * 析构函数，释放所有缓存的连接
     */
    virtual ~DriverPool() {
        while (!m_driverList.empty()) {
            Wrap *p = m_driverList.front();
            m_driverList.pop();
            if (p) {
                delete p;
            }
        }
    }

    /** 获取可用连接，如超出允许的最大连接数，将阻塞等待，直到获得空闲资源 */
    WrapPtr getConnect() noexcept {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_driverList.empty()) {
            if (m_maxSize > 0 && m_count >= m_maxSize) {
                m_cond.wait(lock, [this] { return !m_driverList.empty(); });
            } else {
                m_count++;
                return WrapPtr(new Wrap(m_prototype->clone()), m_closer);
            }
        }
        Wrap *p = m_driverList.front();
        m_driverList.pop();
        return WrapPtr(p, m_closer);
    }

    DriverPtr getPrototype() {
        return m_prototype;
    }

    /** 当前活动的连接数 */
    size_t count() const {
        return m_count;
    }

    /** 当前空闲的资源数 */
    size_t idleCount() const {
        return m_driverList.size();
    }

    /** 释放当前所有的空闲资源 */
    void releaseIdleConnect() {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_driverList.empty()) {
            Wrap *p = m_driverList.front();
            m_driverList.pop();
            m_count--;
            if (p) {
                delete p;
            }
        }
    }

private:
    /** 归还至连接池 */
    void returnDriver(Wrap *p) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (p) {
            if (m_driverList.size() < m_maxIdelSize) {
                m_driverList.push(p);
                m_cond.notify_all();
            } else {
                delete p;
                m_count--;
            }
        } else {
            m_count--;
            HKU_WARN("Trying to return an empty pointer!");
        }
    }

private:
    size_t m_maxSize;                         //允许的最大连接数
    size_t m_maxIdelSize;                     //允许的最大空闲连接数
    size_t m_count;                           //当前活动的连接数
    std::shared_ptr<DriverType> m_prototype;  // 驱动原型
    std::mutex m_mutex;
    std::condition_variable m_cond;
    std::queue<Wrap *> m_driverList;

    class DriverCloser {
    public:
        explicit DriverCloser(DriverPool *pool) : m_pool(pool) {}
        void operator()(Wrap *conn) {
            if (m_pool && conn) {
                m_pool->returnDriver(conn);
            }
        }

    private:
        DriverPool *m_pool;
    };

    DriverCloser m_closer;
};

}  // namespace hku
