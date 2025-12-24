#pragma once

/**
 * @brief Template for optional data with validity flag.
 * Useful for fields that may not always be initialized.
 */
template <typename T>
class maybe {
protected:
    // The actual data
    T data;

    // flag if the data is valid
    bool is_valid;

public:
    /**
     * @brief Construct a new has value object
     * 
     */
    maybe(): 
        data(T()), is_valid(false) 
    {}

    /**
     * @brief Construct a new has value object
     * 
     * @param value 
     */
    maybe(const T& value): 
        data(value), is_valid(true) 
    {}

    /**
     * @brief Clear the validity flag
     * 
     */
    void clear() {
        is_valid = false;

        // clear the data
        data = T();
    }

    /**
     * @brief Set the data
     * 
     * @param T& 
     */
    void set(const T& value) {
        // set the data
        data = value;

        // mark the data as valid
        is_valid = true;
    }

    /**
     * @brief Get the data
     * 
     * @return T& 
     */
    const T& get() const {
        return data;
    }

    /**
     * @brief Get the data
     * 
     * @return T& 
     */
    T& get() {
        return data;
    }

    /**
     * @brief Check if the data is valid
     * 
     * @return true 
     * @return false 
     */
    bool has_value() const {
        return is_valid;
    }
};
